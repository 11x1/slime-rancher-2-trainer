// spawn_scaling: resource node spawn scaling + "More ores" (amount multiplier)
// with per-source control: resource nodes, lightning strikes, cyclones and
// explorer drones.
//
// Strategy: patch the game's data in memory, never the per-roll code.
//   Spawning (group "Spawning"):
//   * ResourceNodeDirectorConfig (ScriptableObject, one per zone/area):
//       _maxActiveNodes            <- vanilla x "max active nodes" (hard-capped)
//       _updateNodesIntervalHours  <- vanilla x respawn_interval / spawn_rate
//   More ores (group "More ores"), per source: apply on/off, advanced on/off.
//     factor = !apply ? 1 : (advanced ? res_<type> : ore_amount_mult)
//   * Resource nodes - ResourceNodeDefinition (: ResourceSpawnerDefinition):
//       _minResources/_maxResources <- vanilla x factor
//       advanced: _resources[i].Weight <- vanilla x res_<type>, count x sum(w*m)/sum(w)
//   * Lightning - LightningStrike prefab component (reached through the weather
//       SpawnTemporaryObjectActivity asset whose Prefab carries it):
//       SpawnOptions[i].Min/MaxQuantity <- vanilla x factor
//       SpawnActivity.SecondsBetweenSpawns (Range) <- vanilla / spawn_rate
//   * Cyclones - SpawnTornadoActivity asset + scene StaticTornadoSpawner:
//       SpawnOptions[i].MinCount/MaxCount (List<struct>) for resource actor
//       types only (slimes/animals untouched) <- vanilla x factor
//       SecondsBetweenSpawns / _min/_maxSpawnDelayGameHours <- vanilla / spawn_rate
//   * Explorer drones - ExplorerDroneConfig (ScriptableObject):
//       _lootTable[i].OutputQuantity.Min/Max and _defaultLootConfig.OutputQuantity
//       (RangeInt objects, shared by DroneDirector's loot dictionary copies)
//       <- vanilla x factor. Gather interval (GenerationIntervalHours) vanilla.
// Originals are stored per object pointer and every value is recomputed from
// them, so nothing is ever applied twice and multipliers of 1.0 restore the
// exact vanilla values. Disabling the module restores all originals.
//
// Two small "patch-on-touch" hooks make sure a node asset is patched before
// the game reads it, even if it was loaded between periodic rescans:
//   ResourceNodeDirector.Awake()                       -> patch its _config
//   ResourceSpawnerDefinition.GetResourcesToSpawn(bool) -> patch `this`
// Both call the original unchanged; they never alter results.
//
// Rolled node lists (what a harvest actually yields): ResourceNodeSpawner.SpawnNode
// stores GetResourcesToSpawn()'s list in ResourceNodeSpawnerModel.resourcesToSpawn
// ONCE, it is saved with the node, and ResourceNode.SpawnResourcesAsync
// instantiates exactly that list on harvest (verified by disassembly, 1.3.0).
// So nodes that already existed (rolled before the change, or loaded from the
// save) keep their old yield. The module therefore also rescales the rolled
// lists of unharvested (READY) nodes in loaded zones, from the list as first
// seen ("base") and the multipliers it was rolled with - never compounding:
//   * settings change / enable / zone load / light pass: every READY node
//     reachable through ResourceNodeDirector.AllResourceDirectors
//   * ResourceNode.SpawnSingleResource (pre-call, first pull of a harvest):
//     the harvested node itself
//   * disable: lists back to their vanilla-equivalent size
// The normal (vacuum) harvest is NOT ResourceNode.SpawnResources (no callers in
// 1.3.0): UpdateForState pops ONE item per SpawnSingleResource (list[0],
// RemoveAt(0)) every _timeToHarvest seconds while the vac touches the node;
// releasing the vac sets the node back to READY with the rest of the list.
// _breakMode nodes start SpawnResourcesAsync inline after one _timeToHarvest
// wait (one item per _resourceSpawnDelaySeconds). Harvest pacing: per node
// instance, the per-item time (_timeToHarvest, or _resourceSpawnDelaySeconds in
// break mode) is scaled by vanillaCount / scaledCount, so a node takes as long
// as its vanilla amount did; the SpawnSingleResource post-hook keeps the
// schedule exact (no per-frame rounding loss, catches up several items/frame).
// A partially harvested list (a tail of what we wrote) is never rescaled.
// A list whose provenance is unknown (loaded from the save) is taken as
// vanilla only if its length lies inside the definition's vanilla
// [_minResources, _maxResources]; otherwise it was already scaled (earlier
// session) and is left as it is until the multiplier changes.
//
// Not touched on purpose:
//   * Any lifetime / despawn value (ResourceNodeDefinition._min/_maxLifetimeHours,
//     LightningStrike.DespawnTime, tornado durations). 1.3.0 has a known freeze
//     when a Resource Detector's target despawns.
//   * ResourceGrowerDefinition (gardens / wild fruit trees), ranch drones (they
//     only move existing items), crates (BreakOnImpact counts are shared
//     ScriptedInt assets) - see NOTES.md.
//   * Live LightningStrike instances: only the prefab component is patched;
//     Unity's Instantiate copies the (patched) serialized SpawnOptions into
//     each new strike. Patching instances as well would double-apply.
//
// SAVE SAFETY (from the dump, game 1.3.0):
//   Resource nodes ARE persisted. Each ResourceNodeSpawner (a fixed spawn
//   point placed in the scene) owns a ResourceNodeSpawnerModel that is saved
//   as Persist.ResourceNodeV01 {ID, ResourceNodeState, DespawnAtWorldTime,
//   ResourceNodeDefinitionId, ResourceNodeVariantIndex, List<int>
//   ResourcesToSpawn}. Consequences:
//     - The number of saved node records equals the number of spawn points,
//       which this module never changes. A higher _maxActiveNodes only lets
//       more of the existing spawn points be occupied at once.
//     - The rolled yield (ResourcesToSpawn) is stored with the node, so nodes
//       spawned while the ore multiplier was active keep their larger yield
//       after the mod is removed; it is a plain list of ints, which vanilla
//       loads at any length.
//     - After uninstall, the director just sees more active nodes than its
//       vanilla max and stops spawning until they despawn by their (vanilla)
//       lifetimes.
//   Weather resources are ordinary loose actors once spawned; explorer drone
//   output goes into the drone cloud storage (capped by the game's own stack
//   limit). The per-zone hard cap and the per-roll yield cap bound how far
//   this can grow. Mod settings live in sr2mods.json, never in the save.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/perf.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/spawn_scaling/scaling_math.h"

namespace sr2::modules {
namespace {

namespace ss = sr2::modules::spawn_scaling;
namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;

constexpr const char* kModule = "spawn_scaling";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kNodeNs = "MonomiPark.SlimeRancher.World.ResourceNode";
constexpr const char* kWorldNs = "MonomiPark.SlimeRancher.World";
constexpr const char* kActivityNs = "MonomiPark.SlimeRancher.Weather.Activity";
constexpr const char* kSrNs = "MonomiPark.SlimeRancher";
constexpr const char* kDroneNs = "MonomiPark.SlimeRancher.Drone";
constexpr const char* kUtilNs = "MonomiPark.SlimeRancher.Util";
// Periodic work (perf). Resources.FindObjectsOfTypeAll walks every loaded object
// and allocates a managed array; the old 2 s rescan did 5 of them in one frame
// (~80 ms -> the periodic stutter). Now:
//   * light pass every kRescanSeconds: ResourceNodeDirector.AllResourceDirectors
//     (static list) -> each _config and its weight map's node definitions. No
//     FindObjectsOfTypeAll, no managed allocation.
//   * patch-on-touch hooks: director Awake, GetResourcesToSpawn, weather activity
//     Spawn, StaticTornadoSpawner.OnEnable.
//   * scan cycle (time-sliced: ONE FindObjectsOfTypeAll class per frame, 5 frames):
//     on enable/disable, kZoneSettleSeconds and kZoneFollowUpSeconds after the
//     director set changed (zone load), and every kSafetyRescanSeconds. Without
//     the director getter every kFallbackRescanSeconds.
//   * settings change: NO scan. Re-apply to the already-tracked objects in one
//     frame (live directors -> configs/defs, weather/drone objects through their
//     GC handles, rolled lists of READY nodes). Only a source switched from off
//     to on needs a scan (its objects are not tracked while off).
constexpr float kRescanSeconds = 2.0f;
constexpr float kSettingsDebounceSeconds = 0.25f;  // min gap between settings re-applies (slider drags)
constexpr int kNodeLogsPerZone = 5;                // Info lines per zone for node spawns / harvests
constexpr size_t kMaxNodeRecords = 4096;           // prune unseen rolled-list records above this
constexpr size_t kMaxRolledListLen = 10000;        // sanity bound for a rolled resource list
// Harvest pacing (per-item time x vanillaCount / scaledCount).
constexpr float kMinPaceSeconds = 0.001f;          // never write a 0 / negative per-item time
constexpr double kMaxCatchUpSeconds = 0.25;        // schedule debt carried over (frame hitch) at most
constexpr int kMaxExtraPullsPerFrame = 8;          // extra SpawnSingleResource calls per frame when behind
constexpr size_t kMaxPaceRecords = 4096;           // prune unseen node-instance pacing records above this
constexpr size_t kMaxHarvestSessions = 256;        // open harvest sessions (log bookkeeping) before a reset
constexpr float kZoneSettleSeconds = 2.0f;        // scan cycle this long after the director set changed...
constexpr float kZoneFollowUpSeconds = 10.0f;     // ...and once more for late-loading assets
constexpr float kSafetyRescanSeconds = 300.0f;
constexpr float kFallbackRescanSeconds = 30.0f;
constexpr float kMinSecondsBetweenWeatherSpawns = 1.0f;  // SpawnActivity.SecondsBetweenSpawns floor
constexpr float kMinTornadoDelayHours = 0.05f;           // StaticTornadoSpawner delay floor
// Il2CppObject header (klass + monitor): IL2CPP field offsets of value-type
// fields include it; inline struct elements in arrays / fields do not.
// Runtime ABI constant, same footing as unity::kArrayDataOffset.
constexpr size_t kObjectHeader = 2 * sizeof(void*);

// Setting keys.
constexpr const char* kSpawnRate = "spawn_rate";
constexpr const char* kMaxNodes = "max_nodes_mult";
constexpr const char* kRespawn = "respawn_interval_mult";
constexpr const char* kHardCap = "hard_cap_nodes";
constexpr const char* kOreAmount = "ore_amount_mult";
constexpr const char* kYieldCap = "max_yield_per_node";
constexpr const char* kKeepHarvestTime = "keep_harvest_time";
constexpr const char* kResPrefix = "res_";
constexpr const char* kGroupMoreOres = "More ores";
constexpr const char* kGroupPerResource = "Per resource (advanced)";

constexpr const char* kLiveTip = "";

// ---- sources of the "More ores" multiplier ----
enum Source : int { kSrcNodes = 0, kSrcLightning, kSrcCyclone, kSrcDrones, kSrcCount };
struct SourceInfo {
    const char* id;     // key part: src_<id>_apply / src_<id>_advanced
    const char* label;  // UI label prefix
    const char* what;   // tooltip text
};
constexpr std::array<SourceInfo, kSrcCount> kSources{{
    {"nodes", "Resource nodes", "resources per harvested ground resource node"},
    {"lightning", "Lightning strikes", "Lightning Mote and Storm Glass per lightning strike"},
    {"cyclone", "Cyclones", "resources per cyclone burst, like Drift Crystal (never slimes)"},
    {"drones", "Explorer drones", "items an explorer drone gathers per collection"},
}};
std::string ApplyKey(int s) { return std::format("src_{}_apply", kSources[s].id); }
std::string AdvancedKey(int s) { return std::format("src_{}_advanced", kSources[s].id); }

// "RadiantOre" -> "Radiant Ore"
std::string PrettyName(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            continue;
        }
        if (i > 0 && c >= 'A' && c <= 'Z') {
            const char p = s[i - 1];
            const bool prevLowerOrDigit = (p >= 'a' && p <= 'z') || (p >= '0' && p <= '9');
            const bool nextLower = i + 1 < s.size() && s[i + 1] >= 'a' && s[i + 1] <= 'z';
            const bool prevUpper = p >= 'A' && p <= 'Z';
            if (prevLowerOrDigit || (prevUpper && nextLower)) {
                if (!out.empty() && out.back() != ' ') out.push_back(' ');
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string SanitizeKey(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    return out;
}

// Snapshot of the settings for one apply pass.
struct Params {
    double spawnRate = 1, maxNodes = 1, respawn = 1, ore = 1;
    int hardCap = 0, yieldCap = 100;
    bool keepHarvestTime = true;  // per-item harvest time x vanillaCount / scaledCount
    std::array<bool, kSrcCount> apply{true, true, true, true};
    std::array<bool, kSrcCount> advanced{false, false, false, false};
};

// Tracked non-node objects (weather + drones).
enum class ObjKind { LightningPrefab, LightningActivity, TornadoActivity, StaticTornado, DroneConfig };

const char* KindName(ObjKind k) {
    switch (k) {
        case ObjKind::LightningPrefab: return "lightning prefab";
        case ObjKind::LightningActivity: return "lightning activity";
        case ObjKind::TornadoActivity: return "tornado activity";
        case ObjKind::StaticTornado: return "static tornado";
        case ObjKind::DroneConfig: return "explorer drone config";
    }
    return "?";
}

Source SourceOf(ObjKind k) {
    switch (k) {
        case ObjKind::LightningPrefab:
        case ObjKind::LightningActivity: return kSrcLightning;
        case ObjKind::TornadoActivity:
        case ObjKind::StaticTornado: return kSrcCyclone;
        case ObjKind::DroneConfig: return kSrcDrones;
    }
    return kSrcNodes;
}

using ClassValueSizeFn = int32_t (*)(Il2CppClass*, uint32_t*);
using ArrayElementSizeFn = int (*)(const Il2CppClass*);
using FieldGetTypeFn = const Il2CppType* (*)(FieldInfo*);
using FieldStaticGetValueFn = void (*)(FieldInfo*, void*);
using WbarrierSetFieldFn = void (*)(Il2CppObject*, void**, void*);

// Strong GC handle owner (move-only). Tracked weather/drone objects are held
// with one so a settings re-apply can reach them without a FindObjectsOfTypeAll
// scan and without touching freed memory.
struct GcRef {
    Il2CppGCHandle h = nullptr;
    GcRef() = default;
    GcRef(const GcRef&) = delete;
    GcRef& operator=(const GcRef&) = delete;
    GcRef(GcRef&& o) noexcept : h(std::exchange(o.h, nullptr)) {}
    GcRef& operator=(GcRef&& o) noexcept {
        if (this != &o) {
            reset();
            h = std::exchange(o.h, nullptr);
        }
        return *this;
    }
    ~GcRef() { reset(); }
    void set(Il2CppObject* o) {
        reset();
        if (o) h = unity::NewGCHandle(o);
    }
    void reset() {
        if (h) unity::FreeGCHandle(std::exchange(h, nullptr));
    }
    Il2CppObject* get() const { return h ? unity::GCHandleTarget(h) : nullptr; }
};

// Inline struct element layout (List<struct> items).
struct StructLayout {
    Il2CppClass* klass = nullptr;
    size_t stride = 0;     // 0 = ask the array class at runtime
    size_t minStride = 0;  // sanity bound from the field offsets we touch
};

class SpawnScaling final : public sr2::core::Module {
public:
    // Destroyed at process exit only: never call into IL2CPP then (GC handles leak on purpose).
    ~SpawnScaling() override {
        for (auto& kv : objs_) kv.second.handle.h = nullptr;
    }
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

    // Called from the hook detours (main thread, inside game code).
    void TouchDirector(Il2CppObject* director);
    void TouchDefinition(Il2CppObject* def);
    void TouchWeather(Il2CppObject* obj, bool staticSpawner);  // activity Spawn / StaticTornadoSpawner.OnEnable
    void OnRolled(Il2CppObject* def, Il2CppObject* list, bool forceMax);  // after GetResourcesToSpawn
    void OnHarvest(Il2CppObject* node);                                  // before ResourceNode.SpawnResources
    // ResourceNode.SpawnSingleResource (one item of a vacuum harvest).
    struct PullStep {
        bool tracked = false;  // BeforePull ran and found a valid node / list
        bool pace = false;     // keep the schedule exact (paced, per-item mode)
        double due = 0;        // _harvestAtTime before the call (when this item was due)
    };
    void BeforePull(Il2CppObject* node, PullStep& st);
    bool AfterPull(Il2CppObject* node, PullStep& st);  // true: another item is due this frame

private:
    struct ConfigRec {
        Il2CppClass* klass = nullptr;
        std::string name;
        int32_t origMax = 0;
        float origInterval = 0;
        int32_t wroteMax = 0;
        float wroteInterval = 0;
        uint64_t seen = 0;
    };
    struct EntryRec {
        void* entry = nullptr;
        float origWeight = 0;
        float wroteWeight = 0;
        std::string resKey;   // settings key ("res_<Name>"), empty if no type
        std::string resName;  // IdentifiableType asset name
    };
    struct DefRec {
        Il2CppClass* klass = nullptr;
        std::string name;
        void* resArray = nullptr;
        int32_t origMin = 0, origMax = 0;
        int32_t wroteMin = 0, wroteMax = 0;
        std::vector<EntryRec> entries;
        uint64_t seen = 0;
        uint64_t appliedGen = 0;  // paramsGen_ of the last ApplyDef (0 = restored / never)
    };
    // One rolled resource list (ResourceNodeSpawnerModel.resourcesToSpawn),
    // keyed by the list object. `base` is the list as rolled / first seen,
    // `mBase` per type the multiplier it already carries (1 = vanilla roll).
    struct NodeTypeCount {
        Il2CppObject* type = nullptr;
        int32_t count = 0;
        double mBase = 1.0;
    };
    enum class NodeOrigin { Rolled, Vanilla, PreScaled, Unknown };
    struct NodeRec {
        std::vector<Il2CppObject*> base;
        std::vector<NodeTypeCount> types;  // first-appearance order
        std::vector<Il2CppObject*> wrote;  // what the list holds now (as written / verified by us)
        std::string defName;
        NodeOrigin origin = NodeOrigin::Unknown;
        uint64_t appliedGen = 0;
        uint64_t sweep = 0;
        double paceRatio = 1.0;  // vanilla-equivalent count / written count (<= 1), for harvest pacing
        bool partial = false;    // partly harvested (a tail of `wrote`): never rescaled again
    };
    // Per ResourceNode instance (reached through a live spawner only): the
    // per-item harvest time we scaled. Instance fields, the prefab is never
    // touched; a value that no longer matches what we wrote is re-baselined.
    struct PaceRec {
        float origT = 0, wroteT = 0;  // _timeToHarvest
        float origD = 0, wroteD = 0;  // _resourceSpawnDelaySeconds
        double ratio = 1.0;
        bool breakMode = false;
        uint64_t sweep = 0;
    };
    // One vacuum harvest of a node (log bookkeeping, SpawnSingleResource).
    struct HarvestSession {
        void* list = nullptr;
        int startLen = 0, expectLen = 0, count = 0, pauses = 0;
        double startTime = -1, lastTime = -1;  // game time (Time.timeAsDouble, derived)
        float vanillaT = 0, pacedT = 0;
        double ratio = 1.0;
        float firstWaitT = 0;  // _timeToHarvest when the harvest started (first item's wait)
        bool rescaled = false, partialBefore = false;
        NodeOrigin origin = NodeOrigin::Unknown;
        std::string defName, types;
    };
    struct NodeStats {
        int seen = 0, rescaled = 0, itemsBefore = 0, itemsAfter = 0, paced = 0;
        double ms = 0;
    };
    // A (min, max) amount pair inside a spawn/loot option.
    struct CountPair {
        void* base = nullptr;      // object holding min/max (or inline element address)
        void** baseLoc = nullptr;  // where the game stores `base` (validated), null for inline elements
        int32_t* pMin = nullptr;
        int32_t* pMax = nullptr;
        int32_t origMin = 0, origMax = 0, wroteMin = 0, wroteMax = 0;
        bool eligible = false;  // a resource (not empty / slime / animal)
        std::string resKey, resName;
    };
    struct FloatSlot {
        float* p = nullptr;
        float orig = 0, wrote = 0;
    };
    struct ObjRec {
        ObjKind kind = ObjKind::LightningPrefab;
        Il2CppClass* klass = nullptr;
        std::string name;
        void* list = nullptr;
        void* items = nullptr;
        size_t count = 0;
        void* range = nullptr;
        std::vector<CountPair> pairs;
        std::vector<FloatSlot> freq;
        float freqFloor = 0;
        uint64_t seen = 0;
        uint64_t appliedGen = 0;
        GcRef handle;  // keeps the managed object valid for settings re-applies
    };

    // ---- setup helpers ----
    bool ResolveWeather(sr2::core::ModuleContext& ctx, std::string& why);
    bool ResolveDrones(sr2::core::ModuleContext& ctx, std::string& why);
    bool InitStructLayout(StructLayout& l, size_t minStride, std::string& why);

    // ---- main-thread only ----
    void Frame(float dt);
    // Time-sliced scan cycle: StartCycle() snapshots the settings, then one
    // ScanStep() per frame (one FindObjectsOfTypeAll class each), FinishCycle()
    // logs the per-cycle summary.
    enum ScanStepId : int { kStepConfigs = 0, kStepDefs, kStepWeather, kStepStaticTornado, kStepDrones, kScanSteps };
    void StartCycle(bool active, bool triggered);
    void ScanStep();
    void FinishCycle();
    void LightPass();  // directors' configs + their node definitions, no FindObjectsOfTypeAll
    // Live directors -> configs + weight-map node definitions with `p`. `force`
    // re-applies definitions even if they already hold values for this gen.
    // Returns the director list signature (count + pointers), 0 on failure.
    uint64_t DirectorPass(const Params& p, int& cfgChanged, int& defChanged, int& cfgNew, int& defNew, size_t& nDirs);
    // Settings change: re-apply to tracked objects only (no scan). False = a
    // scan cycle is needed instead (a source was switched on).
    bool ReapplyTracked();
    std::string TrackedSummary() const;
    std::string ParamsSummary(const Params& p) const;
    Params ReadParams() const;
    double ResourceMult(const std::string& key);
    double SourceFactor(int src, const std::string& resKey, const Params& p);

    ConfigRec& EnsureConfig(Il2CppObject* cfg, bool& isNew);
    DefRec& EnsureDef(Il2CppObject* def, bool& isNew);
    void BaselineConfig(Il2CppObject* cfg, ConfigRec& r);
    void BaselineDef(Il2CppObject* def, DefRec& r);
    bool ConfigMatches(Il2CppObject* cfg, const ConfigRec& r) const;
    bool DefMatches(Il2CppObject* def, const DefRec& r) const;
    bool ApplyConfig(Il2CppObject* cfg, ConfigRec& r, const Params& p);  // true if a value changed
    bool ApplyDef(Il2CppObject* def, DefRec& r, const Params& p);
    bool RestoreConfig(Il2CppObject* cfg, ConfigRec& r);
    bool RestoreDef(Il2CppObject* def, DefRec& r);
    // Per-resource settings are collected while scanning and registered in
    // sorted batches (ores first, drone-only extras after) - see FlushPendingResources.
    void NoteResource(const std::string& key, const std::string& rawName, Il2CppObject* type, bool ore);
    void FlushPendingResources();
    std::string DisplayLabel(const std::string& rawName, Il2CppObject* type);
    bool IsNodeDefClass(Il2CppClass* k) const;

    // ---- rolled node lists ----
    bool ResolveNodeLists(sr2::core::ModuleContext& ctx, std::string& why);
    bool IsIdentType(Il2CppClass* k);
    bool ReadRolledList(void* list, std::vector<Il2CppObject*>& out);
    bool WriteRolledList(void* list, const std::vector<Il2CppObject*>& items);
    const std::pair<std::string, std::string>& TypeNames(Il2CppObject* type);  // {settings key, raw name}
    double NodeTypeMult(Il2CppObject* type, const Params& p, bool active);
    void DefRanges(Il2CppObject* def, int32_t& vMin, int32_t& vMax, int32_t& pMin, int32_t& pMax, std::string& name);
    bool DefPatchedNow(Il2CppObject* def);
    void BaselineNode(NodeRec& r, std::vector<Il2CppObject*>&& items, Il2CppObject* def, NodeOrigin origin,
                      const Params& p, bool active);
    std::vector<Il2CppObject*> NodeTarget(const NodeRec& r, const Params& p, bool active);
    // Brings one rolled list to the target for `p`. Returns true if rewritten.
    bool ProcessNodeList(void* list, Il2CppObject* def, bool active, const Params& p, NodeStats& st,
                         NodeRec** outRec = nullptr, int* outBefore = nullptr);
    // Every READY node of every live director. Logs an Info line when it rescaled
    // something and `log` is set.
    NodeStats NodeSweep(bool active, const char* reason, bool log);
    std::string TypeSummary(const std::vector<Il2CppObject*>& items);
    static const char* OriginName(NodeOrigin o);
    // ---- harvest pacing ----
    bool ResolvePacing(sr2::core::ModuleContext& ctx, std::string& why);
    bool IsNodeObject(Il2CppObject* node, Il2CppObject* model);  // live ResourceNode bound to `model`
    static double PaceRatioOf(const NodeRec& r);
    // Scales (ratio < 1) or restores (ratio 1) one node's per-item harvest time.
    // Returns true while the node is paced.
    bool ApplyPace(Il2CppObject* node, double ratio, const std::string& defName);

    // weather + drones
    bool WantWeather(bool active, const Params& p) const;
    bool WantDrones(bool active, const Params& p) const;
    void ScanWeather(bool active, const Params& p);
    void ScanStaticTornadoes(bool active, const Params& p);
    void ScanDrones(bool active, const Params& p);
    void EraseUnseenObjs(std::initializer_list<ObjKind> kinds, const Params& p);
    bool HandleObj(Il2CppObject* o, ObjKind kind, bool active, bool allowCreate, const Params& p);
    void BaselineObj(Il2CppObject* o, ObjKind kind, ObjRec& r);
    bool ObjMatches(Il2CppObject* o, const ObjRec& r) const;
    bool ApplyObj(ObjRec& r, const Params& p);
    bool RestoreObj(ObjRec& r);
    void* ListItems(void* list) const;
    size_t StrideFor(StructLayout& l, void* items) const;
    bool IsResourceActor(Il2CppObject* type) const;
    void FillResource(CountPair& c, Il2CppObject* type, bool ore);

    // Resolved in Setup.
    Il2CppClass* cfgClass_ = nullptr;
    Il2CppClass* defClass_ = nullptr;  // ResourceNodeDefinition
    size_t offDirConfig_ = 0;    // ResourceNodeDirector._config
    size_t offMaxActive_ = 0;    // ResourceNodeDirectorConfig._maxActiveNodes
    size_t offInterval_ = 0;     // ResourceNodeDirectorConfig._updateNodesIntervalHours
    size_t offResources_ = 0;    // ResourceSpawnerDefinition._resources
    size_t offMinRes_ = 0;       // ResourceSpawnerDefinition._minResources
    size_t offMaxRes_ = 0;       // ResourceSpawnerDefinition._maxResources
    size_t offEntryType_ = 0;    // WeightedResourceEntry.ResourceIdentifiableType
    size_t offEntryWeight_ = 0;  // WeightedResourceEntry.Weight
    bool entryOffsetsOk_ = false;

    ClassValueSizeFn classValueSize_ = nullptr;
    ArrayElementSizeFn arrayElementSize_ = nullptr;

    // Weather (optional; weatherOk_ false -> lightning + cyclone sources inactive).
    bool weatherOk_ = false;
    Il2CppClass* identClass_ = nullptr;          // IdentifiableType
    Il2CppClass* lightningClass_ = nullptr;      // World.LightningStrike
    Il2CppClass* tempActClass_ = nullptr;        // Weather.Activity.SpawnTemporaryObjectActivity
    Il2CppClass* tornadoActClass_ = nullptr;     // Weather.Activity.SpawnTornadoActivity
    Il2CppClass* staticTornadoClass_ = nullptr;  // MonomiPark.SlimeRancher.StaticTornadoSpawner
    size_t offIdentIsPlayer_ = 0, offIdentIsAnimal_ = 0;
    size_t offLsOptions_ = 0;                                 // LightningStrike.SpawnOptions
    size_t offRcResource_ = 0, offRcMin_ = 0, offRcMax_ = 0;  // ResourceChance.Resource/MinQuantity/MaxQuantity
    size_t offActRange_ = 0;                                  // SpawnActivity.SecondsBetweenSpawns
    size_t offActPrefab_ = 0;                                 // SpawnTemporaryObjectActivity.Prefab
    size_t offTornadoOptions_ = 0;                            // SpawnTornadoActivity.SpawnOptions
    size_t offStOptions_ = 0, offStMinDelay_ = 0, offStMaxDelay_ = 0;  // StaticTornadoSpawner
    size_t offRangeMin_ = 0, offRangeMax_ = 0;                // Range.Min/Max
    size_t offSoType_ = 0, offSoMin_ = 0, offSoMax_ = 0;      // PeriodicActorSpawner.SpawnOption, UNBOXED
    mutable StructLayout spawnOptionLayout_;

    // Explorer drones (optional; dronesOk_ false -> drones source inactive, no settings).
    bool dronesOk_ = false;
    Il2CppClass* droneCfgClass_ = nullptr;  // Drone.ExplorerDroneConfig
    size_t offDefaultLoot_ = 0;             // ExplorerDroneConfig._defaultLootConfig (inline struct)
    size_t offLootTable_ = 0;               // ExplorerDroneConfig._lootTable
    size_t offLteItem_ = 0, offLteOutput_ = 0;  // LootTableEntry.Item / OutputQuantity, UNBOXED
    size_t offRiMin_ = 0, offRiMax_ = 0;        // Util.RangeInt.Min/Max
    mutable StructLayout lootLayout_;

    mutable std::unordered_map<Il2CppClass*, size_t> listItemsOff_;  // List<T>._items per generic instance

    // Cross-thread flags.
    std::atomic<bool> enabled_{false};
    std::atomic<bool> dirty_{true};
    int frameId_ = 0;

    // Main-thread state.
    bool applied_ = false;  // module state the assets currently reflect
    float sinceScan_ = 0;
    float sinceFull_ = 0;                          // since the last scan cycle (step)
    float sinceTriggered_ = 1e9f;                  // since the last settings/enable cycle started
    float zoneFullIn_[2] = {-1.0f, -1.0f};         // > 0: countdown to a scan cycle after a zone change
    bool zoneScanPending_ = false;
    uint64_t lastDirSig_ = 0;                      // AllResourceDirectors count + pointers
    // Current scan cycle.
    struct ScanStats {
        int cfgChanged = 0, defChanged = 0, cfgNew = 0, defNew = 0;
        std::array<int, kSrcCount> objChanged{};
        double totalMs = 0, maxMs = 0;
    };
    int scanStep_ = -1;  // next step, -1 = idle
    bool scanActive_ = false, scanTriggered_ = false, scanWasApplied_ = false;
    Params scanParams_;
    ScanStats scan_;
    // ResourceNodeDirector.get_AllResourceDirectors() (static) and the config's weight map (optional).
    using DirectorsFn = Il2CppObject* (*)(const MethodInfo*);
    DirectorsFn directorsFn_ = nullptr;
    const MethodInfo* directorsMi_ = nullptr;
    size_t offWeightMap_ = 0, offWeightNodeDef_ = 0;  // Config._resourceNodeWeightMap, WeightedEntry.NodeDefinition
    bool weightMapOk_ = false;
    uint64_t gen_ = 1;
    std::unordered_map<void*, ConfigRec> configs_;
    std::unordered_map<void*, DefRec> defs_;
    std::unordered_map<void*, ObjRec> objs_;
    std::unordered_set<Il2CppClass*> nodeDefClasses_;
    std::unordered_set<std::string> registeredRes_;
    std::unordered_map<std::string, double> resMultCache_;  // valid during one pass
    bool resCacheValid_ = false;
    Params cached_;
    bool cachedValid_ = false;

    // Per-resource registration bookkeeping.
    struct PendingRes {
        std::string label;  // display label without tag
        bool ore = false;   // seen in nodes / lightning / cyclones (not only in drone loot)
    };
    std::unordered_map<std::string, PendingRes> pendingRes_;
    bool nodesSeenEver_ = false;
    float sinceFirstPass_ = -1;  // < 0 until the first pass
    std::string lastSummaryCounts_;

    // Optional localized display names: IdentifiableType.localizedName -> LocalizedString.GetLocalizedString().
    size_t offLocName_ = 0;
    const MethodInfo* locGetString_ = nullptr;

    // Rolled node lists (optional group; nodeListsOk_ false -> only new rolls scale).
    bool nodeListsOk_ = false;
    Il2CppClass* nodeIdentClass_ = nullptr;   // IdentifiableType
    Il2CppClass* spawnerClass_ = nullptr;     // ResourceNodeSpawner
    Il2CppClass* modelClass_ = nullptr;       // DataModel.ResourceNodeSpawnerModel
    Il2CppClass* nodeClass_ = nullptr;        // ResourceNode
    Il2CppClass* rolledListClass_ = nullptr;  // List<IdentifiableType> (type of resourcesToSpawn)
    size_t offDirSpawners_ = 0;               // ResourceNodeDirector._nodeSpawners
    size_t offSpawnerModel_ = 0;              // ResourceNodeSpawner._model
    size_t offModelState_ = 0, offModelDef_ = 0, offModelList_ = 0;  // nodeState / resourceNodeDefinition / resourcesToSpawn
    size_t offNodeModel_ = 0;                 // ResourceNode._model
    size_t offListItems_ = 0, offListSize_ = 0, offListVersion_ = 0;  // List<IdentifiableType>
    const MethodInfo* listSetCapacity_ = nullptr;
    int32_t nodeStateReady_ = -1;             // ResourceNode.NodeState.READY (read from metadata)
    WbarrierSetFieldFn wbarrier_ = nullptr;   // il2cpp_gc_wbarrier_set_field (optional)
    std::unordered_map<void*, NodeRec> nodes_;
    std::unordered_map<Il2CppClass*, bool> identClassCache_;
    std::unordered_map<Il2CppObject*, std::pair<std::string, std::string>> typeNames_;
    uint64_t paramsGen_ = 1;  // bumped whenever cached_ changes
    uint64_t sweepId_ = 0;
    int spawnLogs_ = 0, harvestLogs_ = 0;  // per zone (reset when the director set changes)
    int partialLogs_ = 0;

    // Harvest pacing (optional group; paceOk_ false -> per-item time stays vanilla).
    bool paceOk_ = false;
    size_t offSpawnerNode_ = 0;                    // ResourceNodeSpawner._attachedResourceNode
    size_t offNodeTime_ = 0;                       // ResourceNode._timeToHarvest (float)
    size_t offNodeDelay_ = 0;                      // ResourceNode._resourceSpawnDelaySeconds (float)
    size_t offNodeBreak_ = 0;                      // ResourceNode._breakMode (bool)
    size_t offNodeHarvestAt_ = 0;                  // ResourceNode._harvestAtTime (double)
    int32_t nodeStateHarvesting_ = -1;             // ResourceNode.NodeState.HARVESTING
    std::unordered_map<void*, PaceRec> paces_;     // keyed by ResourceNode instance
    std::unordered_map<void*, HarvestSession> sessions_;
    std::unordered_map<Il2CppClass*, bool> nodeClassCache_;
    std::unordered_set<std::string> pacedDefsLogged_;
};

SpawnScaling* g_self = nullptr;

using DirectorAwakeFn = void (*)(Il2CppObject*, const MethodInfo*);
using GetResourcesToSpawnFn = Il2CppObject* (*)(Il2CppObject*, bool, const MethodInfo*);
// SpawnTemporaryObjectActivity / SpawnTornadoActivity.Spawn(Vector3, Quaternion):
// on x64 MSVC both structs (12 / 16 bytes) are passed by hidden pointer, so
// they are forwarded untouched as void*.
using ActivitySpawnFn = Il2CppObject* (*)(Il2CppObject*, void*, void*, const MethodInfo*);
using StaticTornadoOnEnableFn = void (*)(Il2CppObject*, const MethodInfo*);
DirectorAwakeFn g_origDirectorAwake = nullptr;
GetResourcesToSpawnFn g_origGetResources = nullptr;
ActivitySpawnFn g_origTempSpawn = nullptr;
ActivitySpawnFn g_origTornadoSpawn = nullptr;
StaticTornadoOnEnableFn g_origStaticTornadoOnEnable = nullptr;
using SpawnResourcesFn = void (*)(Il2CppObject*, const MethodInfo*);
SpawnResourcesFn g_origSpawnResources = nullptr;
using SpawnSingleResourceFn = void (*)(Il2CppObject*, const MethodInfo*);
SpawnSingleResourceFn g_origSpawnSingle = nullptr;

// Mod logic of every detour runs inside a DetourGuard (core/detour_guard.h);
// the game's original is always called outside of it, exactly once.
sr2::core::DetourGuard g_guardDirectorAwake{kModule, "ResourceNodeDirector.Awake"};
sr2::core::DetourGuard g_guardGetResources{kModule, "ResourceSpawnerDefinition.GetResourcesToSpawn"};
sr2::core::DetourGuard g_guardTempSpawn{kModule, "SpawnTemporaryObjectActivity.Spawn"};
sr2::core::DetourGuard g_guardTornadoSpawn{kModule, "SpawnTornadoActivity.Spawn"};
sr2::core::DetourGuard g_guardStaticTornado{kModule, "StaticTornadoSpawner.OnEnable"};
sr2::core::DetourGuard g_guardSpawnResources{kModule, "ResourceNode.SpawnResources"};
sr2::core::DetourGuard g_guardSpawnSingle{kModule, "ResourceNode.SpawnSingleResource"};

bool Mine(Il2CppObject* self) { return g_self && self && main_thread::IsMainThread(); }

void DirectorAwakeDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (Mine(self)) {
        SR2_PERF_SCOPE("hook:spawn_scaling.touch");
        g_guardDirectorAwake.Run([&] { g_self->TouchDirector(self); });
    }
    g_origDirectorAwake(self, mi);
}

Il2CppObject* GetResourcesToSpawnDetour(Il2CppObject* self, bool forceMaxCount, const MethodInfo* mi) {
    const bool mine = Mine(self);
    if (mine) {
        SR2_PERF_SCOPE("hook:spawn_scaling.touch");
        g_guardGetResources.Run([&] { g_self->TouchDefinition(self); });
    }
    Il2CppObject* list = g_origGetResources(self, forceMaxCount, mi);
    if (mine && list) {
        SR2_PERF_SCOPE("hook:spawn_scaling.roll");
        g_guardGetResources.Run([&] { g_self->OnRolled(self, list, forceMaxCount); });
    }
    return list;
}

Il2CppObject* TempActivitySpawnDetour(Il2CppObject* self, void* pos, void* rot, const MethodInfo* mi) {
    if (Mine(self)) {
        SR2_PERF_SCOPE("hook:spawn_scaling.touch");
        g_guardTempSpawn.Run([&] { g_self->TouchWeather(self, false); });
    }
    return g_origTempSpawn(self, pos, rot, mi);
}

Il2CppObject* TornadoActivitySpawnDetour(Il2CppObject* self, void* pos, void* rot, const MethodInfo* mi) {
    if (Mine(self)) {
        SR2_PERF_SCOPE("hook:spawn_scaling.touch");
        g_guardTornadoSpawn.Run([&] { g_self->TouchWeather(self, false); });
    }
    return g_origTornadoSpawn(self, pos, rot, mi);
}

void StaticTornadoOnEnableDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (Mine(self)) {
        SR2_PERF_SCOPE("hook:spawn_scaling.touch");
        g_guardStaticTornado.Run([&] { g_self->TouchWeather(self, true); });
    }
    g_origStaticTornadoOnEnable(self, mi);
}

// Pre-call: the coroutine started by the original enumerates the rolled list,
// so the list is brought to the current multiplier BEFORE it starts. Note: in
// 1.3.0 nothing calls SpawnResources natively (break nodes start the same
// coroutine inline in UpdateForState); kept for any managed / future caller.
void SpawnResourcesDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (Mine(self)) {
        SR2_PERF_SCOPE("hook:spawn_scaling.harvest");
        g_guardSpawnResources.Run([&] { g_self->OnHarvest(self); });
    }
    g_origSpawnResources(self, mi);
}

// One item of a vacuum harvest (called by ResourceNode.UpdateForState while
// HARVESTING, non-break nodes). Pre: first pull of a harvest -> rescale the
// list (nobody enumerates it: the game reads list[0] and RemoveAt(0)) and pace
// the node. Post: keep a paced node's schedule exact (_harvestAtTime = due +
// per-item time instead of now + per-item time) and, when the per-item time is
// shorter than a frame, pop the items that are due right away. The original
// is always called outside the guard; the extra calls run the same game method
// under the same conditions the game checks (HARVESTING, list not empty, same
// frame in which the game saw the vac on the node).
void SpawnSingleResourceDetour(Il2CppObject* self, const MethodInfo* mi) {
    const bool mine = Mine(self);
    SpawnScaling::PullStep st;
    if (mine) {
        SR2_PERF_SCOPE("hook:spawn_scaling.harvest");
        g_guardSpawnSingle.Run([&] { g_self->BeforePull(self, st); });
    }
    g_origSpawnSingle(self, mi);
    if (!mine || !st.tracked) return;
    for (int extra = 0;; ++extra) {
        bool again = false;
        {
            SR2_PERF_SCOPE("hook:spawn_scaling.harvest");
            g_guardSpawnSingle.Run([&] { again = g_self->AfterPull(self, st); });
        }
        if (!again || extra >= kMaxExtraPullsPerFrame) break;
        g_origSpawnSingle(self, mi);
    }
}

// ------------------------------------------------------------------ setup --

bool SpawnScaling::InitStructLayout(StructLayout& l, size_t minStride, std::string& why) {
    l.minStride = minStride;
    l.stride = 0;
    if (classValueSize_ && l.klass) {
        uint32_t align = 0;
        const int32_t vs = classValueSize_(l.klass, &align);
        if (vs > 0) l.stride = static_cast<size_t>(vs);
    }
    if (l.stride != 0 && (l.stride < minStride || l.stride > 0x200)) {
        why = std::format("implausible struct size 0x{:X}", l.stride);
        return false;
    }
    if (l.stride == 0 && !arrayElementSize_) {
        why = "neither il2cpp_class_value_size nor il2cpp_class_array_element_size exported";
        return false;
    }
    return true;
}

bool SpawnScaling::ResolveWeather(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    };

    identClass_ = cls("", "IdentifiableType");
    lightningClass_ = cls(kWorldNs, "LightningStrike");
    Il2CppClass* chance = cls(kWorldNs, "LightningStrike/ResourceChance");
    Il2CppClass* spawnAct = cls(kActivityNs, "SpawnActivity");
    tempActClass_ = cls(kActivityNs, "SpawnTemporaryObjectActivity");
    tornadoActClass_ = cls(kActivityNs, "SpawnTornadoActivity");
    staticTornadoClass_ = cls(kSrNs, "StaticTornadoSpawner");
    spawnOptionLayout_.klass = cls(kWorldNs, "PeriodicActorSpawner/SpawnOption");
    Il2CppClass* range = cls("", "Range");

    size_t soType = 0, soMin = 0, soMax = 0;
    const bool ok = identClass_ && lightningClass_ && chance && spawnAct && tempActClass_ && tornadoActClass_ &&
                    staticTornadoClass_ && spawnOptionLayout_.klass && range &&
                    off(identClass_, "IsPlayer", offIdentIsPlayer_) && off(identClass_, "IsAnimal", offIdentIsAnimal_) &&
                    off(lightningClass_, "SpawnOptions", offLsOptions_) && off(chance, "Resource", offRcResource_) &&
                    off(chance, "MinQuantity", offRcMin_) && off(chance, "MaxQuantity", offRcMax_) &&
                    off(spawnAct, "SecondsBetweenSpawns", offActRange_) && off(tempActClass_, "Prefab", offActPrefab_) &&
                    off(tornadoActClass_, "SpawnOptions", offTornadoOptions_) &&
                    off(staticTornadoClass_, "SpawnOptions", offStOptions_) &&
                    off(staticTornadoClass_, "_minSpawnDelayGameHours", offStMinDelay_) &&
                    off(staticTornadoClass_, "_maxSpawnDelayGameHours", offStMaxDelay_) &&
                    off(range, "Min", offRangeMin_) && off(range, "Max", offRangeMax_) &&
                    off(spawnOptionLayout_.klass, "ActorType", soType) &&
                    off(spawnOptionLayout_.klass, "MinCount", soMin) && off(spawnOptionLayout_.klass, "MaxCount", soMax);
    if (!ok) return false;

    // Value-type field offsets include the object header; inline elements do
    // not. ActorType is the struct's first field, so its offset tells which
    // convention this runtime uses.
    const size_t adj = soType >= kObjectHeader ? kObjectHeader : 0;
    offSoType_ = soType - adj;
    offSoMin_ = soMin - adj;
    offSoMax_ = soMax - adj;
    if (!InitStructLayout(spawnOptionLayout_, std::max({offSoType_ + sizeof(void*), offSoMin_ + 4, offSoMax_ + 4}),
                          why))
        return false;

    SR2_DEBUG("[{}] weather: LightningStrike.SpawnOptions=0x{:X} ResourceChance.Resource/Min/Max=0x{:X}/0x{:X}/0x{:X} "
              "SpawnActivity.SecondsBetweenSpawns=0x{:X} Prefab=0x{:X} SpawnTornadoActivity.SpawnOptions=0x{:X} "
              "StaticTornadoSpawner.SpawnOptions/_min/_maxSpawnDelayGameHours=0x{:X}/0x{:X}/0x{:X} "
              "SpawnOption(unboxed) ActorType/MinCount/MaxCount=0x{:X}/0x{:X}/0x{:X} size=0x{:X}",
              kModule, offLsOptions_, offRcResource_, offRcMin_, offRcMax_, offActRange_, offActPrefab_,
              offTornadoOptions_, offStOptions_, offStMinDelay_, offStMaxDelay_, offSoType_, offSoMin_, offSoMax_,
              spawnOptionLayout_.stride);
    return true;
}

bool SpawnScaling::ResolveDrones(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    std::string err;
    droneCfgClass_ = R.Class(kImage, kDroneNs, "ExplorerDroneConfig", &err);
    lootLayout_.klass = R.Class(kImage, kDroneNs, "ExplorerDroneConfig/LootTableEntry", &err);
    Il2CppClass* rangeInt = R.Class(kImage, kUtilNs, "RangeInt", &err);
    if (!droneCfgClass_ || !lootLayout_.klass || !rangeInt) {
        why = err.empty() ? "explorer drone classes not found" : err;
        return false;
    }
    auto dl = R.FieldOffset(droneCfgClass_, "_defaultLootConfig", &err);
    auto lt = R.FieldOffset(droneCfgClass_, "_lootTable", &err);
    auto item = R.FieldOffset(lootLayout_.klass, "Item", &err);
    auto out = R.FieldOffset(lootLayout_.klass, "OutputQuantity", &err);
    auto mn = R.FieldOffset(rangeInt, "Min", &err);
    auto mx = R.FieldOffset(rangeInt, "Max", &err);
    if (!dl || !lt || !item || !out || !mn || !mx) {
        why = err.empty() ? "explorer drone fields not found" : err;
        return false;
    }
    offDefaultLoot_ = *dl;
    offLootTable_ = *lt;
    const size_t adj = *item >= kObjectHeader ? kObjectHeader : 0;  // Item is the first field
    offLteItem_ = *item - adj;
    offLteOutput_ = *out - adj;
    offRiMin_ = *mn;
    offRiMax_ = *mx;
    if (!InitStructLayout(lootLayout_, std::max(offLteItem_, offLteOutput_) + sizeof(void*), why)) return false;
    SR2_DEBUG("[{}] drones: ExplorerDroneConfig._defaultLootConfig=0x{:X} _lootTable=0x{:X} LootTableEntry(unboxed) "
              "Item/OutputQuantity=0x{:X}/0x{:X} size=0x{:X} RangeInt.Min/Max=0x{:X}/0x{:X}",
              kModule, offDefaultLoot_, offLootTable_, offLteItem_, offLteOutput_, lootLayout_.stride, offRiMin_,
              offRiMax_);
    return true;
}

// Rolled-list access: director -> spawners -> model.resourcesToSpawn, the
// harvested node's model, List<IdentifiableType> internals (by name on the
// generic instance taken from the field's own type) and NodeState.READY (read
// from metadata, not hardcoded).
bool SpawnScaling::ResolveNodeLists(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    };
    Il2CppClass* director = cls(kNodeNs, "ResourceNodeDirector");
    nodeIdentClass_ = cls("", "IdentifiableType");
    spawnerClass_ = cls("", "ResourceNodeSpawner");
    modelClass_ = cls("MonomiPark.SlimeRancher.DataModel", "ResourceNodeSpawnerModel");
    nodeClass_ = cls("", "ResourceNode");
    Il2CppClass* stateEnum = cls("", "ResourceNode/NodeState");
    if (!director || !nodeIdentClass_ || !spawnerClass_ || !modelClass_ || !nodeClass_ || !stateEnum) return false;
    if (!off(director, "_nodeSpawners", offDirSpawners_) || !off(spawnerClass_, "_model", offSpawnerModel_) ||
        !off(modelClass_, "nodeState", offModelState_) || !off(modelClass_, "resourceNodeDefinition", offModelDef_) ||
        !off(modelClass_, "resourcesToSpawn", offModelList_) || !off(nodeClass_, "_model", offNodeModel_))
        return false;

    HMODULE ga = sr2::il2cpp::api().module;
    auto fieldType = ga ? reinterpret_cast<FieldGetTypeFn>(GetProcAddress(ga, "il2cpp_field_get_type")) : nullptr;
    auto staticGet =
        ga ? reinterpret_cast<FieldStaticGetValueFn>(GetProcAddress(ga, "il2cpp_field_static_get_value")) : nullptr;
    wbarrier_ = ga ? reinterpret_cast<WbarrierSetFieldFn>(GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field")) : nullptr;
    auto classFromType = sr2::il2cpp::api().il2cpp_class_from_type;
    if (!fieldType || !staticGet || !classFromType) {
        why = "il2cpp_field_get_type / il2cpp_field_static_get_value / il2cpp_class_from_type not exported";
        return false;
    }

    // List<IdentifiableType>: the declared type of resourcesToSpawn.
    std::string err;
    FieldInfo* listField = R.Field(modelClass_, "resourcesToSpawn", &err);
    const Il2CppType* lt = listField ? fieldType(listField) : nullptr;
    rolledListClass_ = lt ? classFromType(lt) : nullptr;
    if (!rolledListClass_) {
        why = err.empty() ? "type of resourcesToSpawn not resolved" : err;
        return false;
    }
    if (!off(rolledListClass_, "_items", offListItems_) || !off(rolledListClass_, "_size", offListSize_) ||
        !off(rolledListClass_, "_version", offListVersion_))
        return false;
    listSetCapacity_ = R.Method(rolledListClass_, "set_Capacity", 1, &err);
    if (!listSetCapacity_) {
        why = err.empty() ? "List<IdentifiableType>.set_Capacity not found" : err;
        return false;
    }

    // NodeState.READY: literal field value from metadata.
    FieldInfo* ready = R.Field(stateEnum, "READY", &err);
    int64_t v = -1;
    std::string terr;
    if (!ready || !sr2::core::Try([&] { staticGet(ready, &v); }, &terr)) {
        why = !terr.empty() ? terr : (err.empty() ? "NodeState.READY not found" : err);
        return false;
    }
    nodeStateReady_ = static_cast<int32_t>(v & 0xFFFFFFFF);
    if (nodeStateReady_ < 0 || nodeStateReady_ > 64) {
        why = std::format("implausible NodeState.READY value {}", nodeStateReady_);
        return false;
    }
    SR2_DEBUG("[{}] rolled lists: Director._nodeSpawners=0x{:X} Spawner._model=0x{:X} Model.nodeState/def/list="
              "0x{:X}/0x{:X}/0x{:X} Node._model=0x{:X} List._items/_size/_version=0x{:X}/0x{:X}/0x{:X} READY={} "
              "wbarrier={}",
              kModule, offDirSpawners_, offSpawnerModel_, offModelState_, offModelDef_, offModelList_, offNodeModel_,
              offListItems_, offListSize_, offListVersion_, nodeStateReady_, wbarrier_ ? "yes" : "no");
    return true;
}

// Harvest pacing: per-instance ResourceNode timing fields, the spawner's
// attached node and NodeState.HARVESTING. Field types are checked through
// il2cpp_type_get_type (float / double / bool) when exported.
bool SpawnScaling::ResolvePacing(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    HMODULE ga = sr2::il2cpp::api().module;
    auto fieldType = ga ? reinterpret_cast<FieldGetTypeFn>(GetProcAddress(ga, "il2cpp_field_get_type")) : nullptr;
    auto typeGetType = ga ? reinterpret_cast<int (*)(const Il2CppType*)>(GetProcAddress(ga, "il2cpp_type_get_type"))
                          : nullptr;
    auto staticGet =
        ga ? reinterpret_cast<FieldStaticGetValueFn>(GetProcAddress(ga, "il2cpp_field_static_get_value")) : nullptr;
    // IL2CPP_TYPE_BOOLEAN / R4 / R8 / CLASS (Il2CppTypeEnum, ECMA-335 element types).
    constexpr int kTypeBool = 0x02, kTypeR4 = 0x0c, kTypeR8 = 0x0d, kTypeClass = 0x12;
    auto field = [&](Il2CppClass* k, const char* name, int wantType, size_t& out) -> bool {
        std::string err;
        FieldInfo* f = k ? R.Field(k, name, &err) : nullptr;
        auto o = f ? R.FieldOffset(k, name, &err) : std::nullopt;
        if (!f || !o) {
            why = err.empty() ? std::format("field {} not found", name) : err;
            return false;
        }
        if (fieldType && typeGetType) {
            const Il2CppType* t = fieldType(f);
            const int tt = t ? typeGetType(t) : -1;
            if (tt != wantType) {
                why = std::format("field {} has type 0x{:X}, expected 0x{:X}", name, tt, wantType);
                return false;
            }
        }
        out = *o;
        return true;
    };
    if (!field(spawnerClass_, "_attachedResourceNode", kTypeClass, offSpawnerNode_) ||
        !field(nodeClass_, "_timeToHarvest", kTypeR4, offNodeTime_) ||
        !field(nodeClass_, "_resourceSpawnDelaySeconds", kTypeR4, offNodeDelay_) ||
        !field(nodeClass_, "_breakMode", kTypeBool, offNodeBreak_) ||
        !field(nodeClass_, "_harvestAtTime", kTypeR8, offNodeHarvestAt_))
        return false;
    std::string err;
    Il2CppClass* stateEnum = R.Class(kImage, "", "ResourceNode/NodeState", &err);
    FieldInfo* harvesting = stateEnum ? R.Field(stateEnum, "HARVESTING", &err) : nullptr;
    int64_t v = -1;
    std::string terr;
    if (!harvesting || !staticGet || !sr2::core::Try([&] { staticGet(harvesting, &v); }, &terr)) {
        why = !terr.empty() ? terr : (err.empty() ? "NodeState.HARVESTING not found" : err);
        return false;
    }
    nodeStateHarvesting_ = static_cast<int32_t>(v & 0xFFFFFFFF);
    if (nodeStateHarvesting_ < 0 || nodeStateHarvesting_ > 64 || nodeStateHarvesting_ == nodeStateReady_) {
        why = std::format("implausible NodeState.HARVESTING value {}", nodeStateHarvesting_);
        return false;
    }
    SR2_DEBUG("[{}] harvest pacing: Spawner._attachedResourceNode=0x{:X} Node._timeToHarvest/_resourceSpawnDelaySeconds/"
              "_breakMode/_harvestAtTime=0x{:X}/0x{:X}/0x{:X}/0x{:X} HARVESTING={} (types {})",
              kModule, offSpawnerNode_, offNodeTime_, offNodeDelay_, offNodeBreak_, offNodeHarvestAt_,
              nodeStateHarvesting_, fieldType && typeGetType ? "checked" : "not checked");
    return true;
}

bool SpawnScaling::Setup(sr2::core::ModuleContext& ctx) {
    // -- classes / offsets (names only) --
    Il2CppClass* director = ctx.Class(kImage, kNodeNs, "ResourceNodeDirector");
    cfgClass_ = ctx.Class(kImage, kNodeNs, "ResourceNodeDirectorConfig");
    Il2CppClass* spawnerDef = ctx.Class(kImage, "", "ResourceSpawnerDefinition");
    defClass_ = ctx.Class(kImage, "", "ResourceNodeDefinition");
    if (!director || !cfgClass_ || !spawnerDef || !defClass_) return false;

    if (!ctx.FieldOffset(director, "_config", offDirConfig_)) return false;
    if (!ctx.FieldOffset(cfgClass_, "_maxActiveNodes", offMaxActive_)) return false;
    if (!ctx.FieldOffset(cfgClass_, "_updateNodesIntervalHours", offInterval_)) return false;
    if (!ctx.FieldOffset(spawnerDef, "_resources", offResources_)) return false;
    if (!ctx.FieldOffset(spawnerDef, "_minResources", offMinRes_)) return false;
    if (!ctx.FieldOffset(spawnerDef, "_maxResources", offMaxRes_)) return false;

    // Optional, for the cheap periodic pass: the static director registry
    // (filled in Awake, emptied in OnDestroy) and the config's weight map.
    {
        std::string err;
        auto& R = ctx.resolver();
        if (const MethodInfo* m = R.Method(director, "get_AllResourceDirectors", 0, &err)) {
            if (void* fn = R.MethodPointer(m, &err)) {
                directorsFn_ = reinterpret_cast<DirectorsFn>(fn);
                directorsMi_ = m;
            }
        }
        Il2CppClass* we = R.Class(kImage, kNodeNs, "ResourceNodeDirectorConfig/WeightedEntry", &err);
        auto wm = R.FieldOffset(cfgClass_, "_resourceNodeWeightMap", &err);
        auto nd = we ? R.FieldOffset(we, "NodeDefinition", &err) : std::nullopt;
        if (wm && nd) {
            offWeightMap_ = *wm;
            offWeightNodeDef_ = *nd;
            weightMapOk_ = true;
        }
        if (!directorsFn_)
            SR2_WARN("[{}] ResourceNodeDirector.AllResourceDirectors unavailable ({}); full rescan every {:.0f} s instead",
                     kModule, err, kFallbackRescanSeconds);
        else
            SR2_DEBUG("[{}] periodic pass via AllResourceDirectors (weight map {})", kModule,
                      weightMapOk_ ? "yes" : "no");
    }

    // Nested class (IL2CPP registers nested types as "Outer/Inner"). Optional:
    // without it only the advanced (per-resource) mode for nodes is unavailable.
    {
        std::string err;
        Il2CppClass* entryClass =
            ctx.resolver().Class(kImage, "", "ResourceSpawnerDefinition/WeightedResourceEntry", &err);
        if (entryClass) {
            auto t = ctx.resolver().FieldOffset(entryClass, "ResourceIdentifiableType", &err);
            auto w = ctx.resolver().FieldOffset(entryClass, "Weight", &err);
            if (t && w) {
                offEntryType_ = *t;
                offEntryWeight_ = *w;
                entryOffsetsOk_ = true;
            }
        }
        if (!entryOffsetsOk_)
            SR2_WARN("[{}] WeightedResourceEntry not resolved ({}); per-resource mode for nodes disabled", kModule, err);
    }

    SR2_DEBUG("[{}] offsets: Director._config=0x{:X} Config._maxActiveNodes=0x{:X} _updateNodesIntervalHours=0x{:X} "
              "Def._resources=0x{:X} _minResources=0x{:X} _maxResources=0x{:X} Entry.Type=0x{:X} Entry.Weight=0x{:X}",
              kModule, offDirConfig_, offMaxActive_, offInterval_, offResources_, offMinRes_, offMaxRes_,
              offEntryType_, offEntryWeight_);

    // Struct sizes for List<struct> access: standard IL2CPP exports, by name.
    if (HMODULE ga = sr2::il2cpp::api().module) {
        classValueSize_ = reinterpret_cast<ClassValueSizeFn>(GetProcAddress(ga, "il2cpp_class_value_size"));
        arrayElementSize_ = reinterpret_cast<ArrayElementSizeFn>(GetProcAddress(ga, "il2cpp_class_array_element_size"));
    }

    // -- optional: localized resource names for the per-resource labels --
    {
        std::string err;
        Il2CppClass* ident = ctx.resolver().Class(kImage, "", "IdentifiableType", &err);
        Il2CppClass* ls = ctx.resolver().Class("Unity.Localization", "UnityEngine.Localization", "LocalizedString", &err);
        auto off = ident ? ctx.resolver().FieldOffset(ident, "localizedName", &err) : std::nullopt;
        const MethodInfo* m = ls ? ctx.resolver().Method(ls, "GetLocalizedString", 0, &err) : nullptr;
        if (off && m) {
            offLocName_ = *off;
            locGetString_ = m;
        } else {
            SR2_DEBUG("[{}] localized resource names unavailable ({}); using asset names", kModule, err);
        }
    }

    // -- optional sources: failure only disables that source --
    {
        std::string why;
        weatherOk_ = ResolveWeather(ctx, why);
        if (!weatherOk_) SR2_WARN("[{}] weather sources (lightning, cyclones) not supported: {}", kModule, why);
    }
    {
        std::string why;
        dronesOk_ = ResolveDrones(ctx, why);
        if (!dronesOk_) SR2_WARN("[{}] explorer drone source not supported: {}", kModule, why);
    }
    {
        std::string why;
        nodeListsOk_ = ResolveNodeLists(ctx, why);
        if (!nodeListsOk_)
            SR2_WARN("[{}] rescaling of already-rolled node yields not supported ({}); only nodes spawned after a "
                     "change get the new amount",
                     kModule, why);
    }

    // -- patch-on-touch hooks (class-specific method bodies, not shared getters) --
    if (!ctx.HookMethod("ResourceNodeDirector.Awake", kImage, kNodeNs, "ResourceNodeDirector", "Awake", 0,
                        reinterpret_cast<void*>(&DirectorAwakeDetour), reinterpret_cast<void**>(&g_origDirectorAwake)))
        return false;
    if (!ctx.HookMethod("ResourceSpawnerDefinition.GetResourcesToSpawn", kImage, "", "ResourceSpawnerDefinition",
                        "GetResourcesToSpawn", 1, reinterpret_cast<void*>(&GetResourcesToSpawnDetour),
                        reinterpret_cast<void**>(&g_origGetResources)))
        return false;
    // Optional weather patch-on-touch hooks (unique bodies in the 1.3.0 dump:
    // 0x66DB30, 0x66DBF0, 0x640BF0). Missing ones only mean the scan cycles
    // (zone change / settings / safety) pick those objects up instead.
    if (weatherOk_) {
        int n = 0;
        n += ctx.HookMethod("SpawnTemporaryObjectActivity.Spawn", kImage, kActivityNs, "SpawnTemporaryObjectActivity",
                            "Spawn", 2, reinterpret_cast<void*>(&TempActivitySpawnDetour),
                            reinterpret_cast<void**>(&g_origTempSpawn))
                 ? 1
                 : 0;
        n += ctx.HookMethod("SpawnTornadoActivity.Spawn", kImage, kActivityNs, "SpawnTornadoActivity", "Spawn", 2,
                            reinterpret_cast<void*>(&TornadoActivitySpawnDetour),
                            reinterpret_cast<void**>(&g_origTornadoSpawn))
                 ? 1
                 : 0;
        n += ctx.HookMethod("StaticTornadoSpawner.OnEnable", kImage, kSrNs, "StaticTornadoSpawner", "OnEnable", 0,
                            reinterpret_cast<void*>(&StaticTornadoOnEnableDetour),
                            reinterpret_cast<void**>(&g_origStaticTornadoOnEnable))
                 ? 1
                 : 0;
        if (n < 3)
            SR2_WARN("[{}] only {}/3 weather patch-on-touch hooks installed (scan cycles still cover them)", kModule, n);
    }
    // Optional harvest hook (unique body 0x53B990 in the 1.3.0 dump): rescales
    // the harvested node's rolled list right before it is spawned. Without it
    // the periodic node sweep still covers nodes in loaded zones. (No native
    // caller in 1.3.0; the vacuum harvest goes through SpawnSingleResource.)
    if (nodeListsOk_ &&
        !ctx.HookMethod("ResourceNode.SpawnResources", kImage, "", "ResourceNode", "SpawnResources", 0,
                        reinterpret_cast<void*>(&SpawnResourcesDetour), reinterpret_cast<void**>(&g_origSpawnResources)))
        SR2_WARN("[{}] ResourceNode.SpawnResources hook not installed; existing nodes are rescaled by the periodic "
                 "sweep only",
                 kModule);
    // Harvest pacing + the real harvest path (unique body 0x53BA90 in the 1.3.0
    // dump; its only caller is ResourceNode.UpdateForState). Optional: without
    // it the per-item time is still scaled by the sweep, only the schedule is
    // not corrected for frame rounding and there are no 'node harvested' lines.
    if (nodeListsOk_) {
        std::string why;
        paceOk_ = ResolvePacing(ctx, why);
        if (!paceOk_)
            SR2_WARN("[{}] harvest pacing not supported ({}); scaled nodes take longer to harvest", kModule, why);
        else if (!ctx.HookMethod("ResourceNode.SpawnSingleResource", kImage, "", "ResourceNode", "SpawnSingleResource",
                                 0, reinterpret_cast<void*>(&SpawnSingleResourceDetour),
                                 reinterpret_cast<void**>(&g_origSpawnSingle)))
            SR2_WARN("[{}] ResourceNode.SpawnSingleResource hook not installed; harvest pacing without schedule "
                     "correction, no 'node harvested' lines",
                     kModule);
    }

    // -- settings --
    using settings::Def;
    using settings::Kind;
    auto reg = [](const std::string& key, const std::string& label, const char* group, Kind kind, double def,
                  double mn, double mx, double step, std::string tip) {
        Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.group = group;
        d.kind = kind;
        d.def = def;
        d.min = mn;
        d.max = mx;
        d.step = step;
        d.tooltip = std::move(tip);
        settings::Register(d);
    };
    reg(kSpawnRate, "Spawn rate multiplier", "Spawning", Kind::Float, 1.0, 0.25, 10.0, 0.25,
        std::string("How often empty spawn points refill. Also speeds up lightning and cyclones while their "
                    "source is applied.") + kLiveTip);
    reg(kMaxNodes, "Max active nodes multiplier", "Spawning", Kind::Float, 1.0, 0.5, 5.0, 0.25,
        std::string("How many resource nodes a zone keeps at once. Limited by its spawn points and the hard "
                    "cap.") + kLiveTip);
    reg(kRespawn, "Respawn interval multiplier", "Spawning", Kind::Float, 1.0, 0.1, 5.0, 0.1,
        std::string("Time between node refills. Below 1 = faster, above 1 = slower.") + kLiveTip);
    reg(kHardCap, "Hard cap: active nodes per zone", "Spawning", Kind::Int, 0, 0, 500, 1,
        std::string("Most active nodes per zone after scaling. 0 = auto (5x the zone's normal amount)."));
    reg(kOreAmount, "Ore amount multiplier", kGroupMoreOres, Kind::Float, 1.0, 0.5, 10.0, 0.25,
        std::string("Resources per roll for every source below that is applied and not in advanced mode.") +
            kLiveTip);
    reg(kYieldCap, "Max resources per roll", kGroupMoreOres, Kind::Int, 100, 1, 500, 1,
        std::string("Limit for one scaled roll, for performance and save size. Never below the normal "
                    "amount."));
    if (paceOk_)
        reg(kKeepHarvestTime, "Resource nodes: keep vanilla harvest time", kGroupMoreOres, Kind::Bool, 1.0, 0, 1, 1,
            std::string("A scaled node gives its items faster, so harvesting takes as long as normal. Off: "
                        "each item takes the normal time.") + kLiveTip);
    for (int s = 0; s < kSrcCount; ++s) {
        if ((s == kSrcLightning || s == kSrcCyclone) && !weatherOk_) continue;
        if (s == kSrcDrones && !dronesOk_) continue;
        reg(ApplyKey(s), std::format("{}: apply multiplier", kSources[s].label), kGroupMoreOres, Kind::Bool, 1.0, 0, 1,
            1,
            std::format("Scales {}. Off: this source stays normal.{}", kSources[s].what, kLiveTip));
        reg(AdvancedKey(s), std::format("{}: advanced (per resource)", kSources[s].label), kGroupMoreOres, Kind::Bool,
            0.0, 0, 1, 1,
            std::format("Uses the multipliers in '{}' instead of the ore amount multiplier.{}",
                        kGroupPerResource, kLiveTip));
        // Visibility parent (settings::Def::parent): advanced mode only matters while the source is scaled.
        settings::SetParent(kModule, AdvancedKey(s), ApplyKey(s));
    }
    if (paceOk_) settings::SetParent(kModule, kKeepHarvestTime, ApplyKey(kSrcNodes));
    {  // the global amount and its cap only matter while some source is scaled
        std::vector<std::string> applied;
        for (int s = 0; s < kSrcCount; ++s) applied.push_back(ApplyKey(s));
        settings::SetAnyParent(kModule, kOreAmount, applied);
        settings::SetAnyParent(kModule, kYieldCap, applied);
    }
    // Units after the slider values (UI only).
    for (const char* k : {kSpawnRate, kMaxNodes, kRespawn, kOreAmount}) settings::SetDisplay(kModule, k, "x");
    settings::SetDisplay(kModule, kHardCap, "", "auto");

    settings::OnChange(kModule, [this](const std::string&) { dirty_.store(true); });

    frameId_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Frame(dt); });
    if (frameId_ == 0) return ctx.Fail("main_thread::AddFrameCallback failed");

    g_self = this;
    SR2_INFO("[{}] setup ok: nodes (ResourceNodeDirectorConfig + ResourceNodeDefinition{}){}{}", kModule,
             nodeListsOk_ ? " + rolled yields of existing nodes" : "",
             weatherOk_ ? ", lightning (LightningStrike prefab), cyclones (SpawnTornadoActivity, StaticTornadoSpawner)"
                        : "",
             dronesOk_ ? ", explorer drones (ExplorerDroneConfig)" : "");
    return true;
}

void SpawnScaling::OnEnable() {
    enabled_.store(true);
    dirty_.store(true);
}

void SpawnScaling::OnDisable() {
    enabled_.store(false);
    dirty_.store(true);  // the next main-thread frame restores every original
}

// ---------------------------------------------------------------- per frame --

void SpawnScaling::Frame(float dt) {
    // A fault inside one of our detours marks the module faulted (DetourGuard):
    // stop all periodic patch work too.
    if (sr2::core::ModuleFaulted(kModule)) return;
    const bool active = enabled_.load();
    sinceScan_ += dt;
    sinceFull_ += dt;
    sinceTriggered_ += dt;
    if (sinceFirstPass_ >= 0) sinceFirstPass_ += dt;
    for (float& left : zoneFullIn_) {
        if (left > 0 && (left -= dt) <= 0) {
            left = -1.0f;
            zoneScanPending_ = true;
        }
    }

    const bool idle = scanStep_ < 0;
    if (idle && !active && !applied_) {  // disabled and nothing patched: nothing to do
        dirty_.store(false);
        return;
    }
    if (active != applied_) {
        // Enable/disable: right away (also restarts a running cycle). On
        // disable the rolled lists of unharvested nodes go back first.
        dirty_.store(false);
        if (!active) NodeSweep(false, "disable", true);
        StartCycle(active, true);
        sinceTriggered_ = 0;
    } else if (idle && active && sinceTriggered_ >= kSettingsDebounceSeconds && dirty_.exchange(false)) {
        // Settings changed: re-apply to what is already tracked (one frame, no
        // FindObjectsOfTypeAll). A source switched on needs a scan instead.
        if (!ReapplyTracked()) StartCycle(true, true);
        sinceTriggered_ = 0;
    } else if (idle && active &&
               (zoneScanPending_ || sinceFull_ >= (directorsFn_ ? kSafetyRescanSeconds : kFallbackRescanSeconds))) {
        StartCycle(true, false);  // zone change / safety net
    }
    if (scanStep_ >= 0) {
        ScanStep();  // one FindObjectsOfTypeAll class per frame
        sinceScan_ = sinceFull_ = 0;
        return;
    }
    if (active && directorsFn_ && sinceScan_ >= kRescanSeconds) {
        sinceScan_ = 0;
        LightPass();
    }
}

uint64_t SpawnScaling::DirectorPass(const Params& p, int& cfgChanged, int& defChanged, int& cfgNew, int& defNew,
                                    size_t& nDirs) {
    nDirs = 0;
    if (!directorsFn_) return 0;
    Il2CppObject* list = nullptr;
    std::string err;
    if (!sr2::core::Try([&] { list = directorsFn_(directorsMi_); }, &err)) {
        SR2_WARN("[{}] AllResourceDirectors failed ({}); full rescan every {:.0f} s instead", kModule, err,
                 kFallbackRescanSeconds);
        directorsFn_ = nullptr;
        return 0;
    }
    const size_t n = unity::ListCount(list);
    nDirs = n;
    uint64_t sig = n + 1;  // never 0
    resMultCache_.clear();
    resCacheValid_ = true;
    for (size_t i = 0; i < n; ++i) {
        auto* dir = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!dir) continue;
        sig = sig * 1000003ull ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dir));
        if (!unity::IsAlive(dir)) continue;
        auto* cfg = unity::Field<Il2CppObject*>(dir, offDirConfig_);
        if (!cfg) continue;
        bool isNew = false;
        ConfigRec& r = EnsureConfig(cfg, isNew);
        if (isNew) ++cfgNew;
        if (ApplyConfig(cfg, r, p)) ++cfgChanged;
        if (!weightMapOk_) continue;
        void* arr = unity::Field<void*>(cfg, offWeightMap_);
        const size_t m = unity::ArrayLength(arr);
        for (size_t j = 0; j < m; ++j) {
            void* e = unity::ArrayAt(arr, j);
            auto* def = e ? unity::Field<Il2CppObject*>(e, offWeightNodeDef_) : nullptr;
            if (!def) continue;
            auto it = defs_.find(def);
            if (it != defs_.end() && it->second.appliedGen == paramsGen_ && DefMatches(def, it->second))
                continue;  // already holds this generation's values
            bool dNew = false;
            DefRec& d = EnsureDef(def, dNew);
            if (dNew) ++defNew;
            if (ApplyDef(def, d, p)) ++defChanged;
        }
    }
    resCacheValid_ = false;
    if (sig == 0) sig = 1;
    return sig;
}

void SpawnScaling::LightPass() {
    if (!applied_ || !cachedValid_ || !directorsFn_) return;
    int cfgChanged = 0, defChanged = 0, cfgNew = 0, defNew = 0;
    size_t n = 0;
    const uint64_t sig = DirectorPass(cached_, cfgChanged, defChanged, cfgNew, defNew, n);
    if (sig == 0) return;
    if (!defs_.empty()) nodesSeenEver_ = true;
    if (cfgChanged || defChanged || cfgNew || defNew)
        SR2_INFO("[{}] light pass: {} directors | patched {} configs ({} new), {} node definitions ({} new)", kModule, n,
                 cfgChanged, cfgNew, defChanged, defNew);
    if (sig != lastDirSig_) {
        lastDirSig_ = sig;
        zoneFullIn_[0] = kZoneSettleSeconds;
        zoneFullIn_[1] = kZoneFollowUpSeconds;
        spawnLogs_ = harvestLogs_ = partialLogs_ = 0;  // node spawn / harvest Info lines: fresh budget per zone
        SR2_DEBUG("[{}] resource director set changed ({} now) - full rescan in {:.0f} s and {:.0f} s", kModule, n,
                  kZoneSettleSeconds, kZoneFollowUpSeconds);
    }
    // Rolled lists of READY nodes (zone loads, nodes loaded from the save, nodes
    // rolled while the module was off). Already-matching lists cost a compare.
    NodeSweep(true, "zone load / nodes not yet scaled", true);
    FlushPendingResources();
}

bool SpawnScaling::ReapplyTracked() {
    if (!directorsFn_) return false;  // no cheap path to the node assets: scan
    const Params p = ReadParams();
    if (cachedValid_) {
        // Weather / drone objects of a source are dropped while it is off, so
        // switching one on needs a scan. Node definitions stay tracked.
        for (int s = kSrcLightning; s < kSrcCount; ++s)
            if (p.apply[s] && !cached_.apply[s]) return false;
    }
    LARGE_INTEGER q0{}, q1{}, qf{};
    QueryPerformanceCounter(&q0);
    cached_ = p;
    cachedValid_ = true;
    ++paramsGen_;

    int cfgChanged = 0, defChanged = 0, cfgNew = 0, defNew = 0;
    size_t nDirs = 0;
    DirectorPass(p, cfgChanged, defChanged, cfgNew, defNew, nDirs);

    std::array<int, kSrcCount> objChanged{};
    resMultCache_.clear();
    resCacheValid_ = true;
    for (auto it = objs_.begin(); it != objs_.end();) {
        ObjRec& r = it->second;
        Il2CppObject* o = r.handle.get();
        if (!o) {  // no GC handle: the next scan / patch-on-touch covers it
            ++it;
            continue;
        }
        if (o != it->first || !unity::IsAlive(o)) {
            it = objs_.erase(it);
            continue;
        }
        const int src = SourceOf(r.kind);
        if (!p.apply[src]) {  // source switched off: write vanilla back, stop tracking
            if (ObjMatches(o, r) && RestoreObj(r)) ++objChanged[src];
            it = objs_.erase(it);
            continue;
        }
        if (!ObjMatches(o, r)) {
            SR2_DEBUG("[{}] {} {} changed externally, re-baselining", kModule, KindName(r.kind), r.name);
            BaselineObj(o, r.kind, r);
        }
        if (ApplyObj(r, p)) ++objChanged[src];
        ++it;
    }
    resCacheValid_ = false;

    const NodeStats ns = NodeSweep(true, "settings", false);
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const double ms = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 /
                      static_cast<double>(qf.QuadPart > 0 ? qf.QuadPart : 1);
    std::string changed = std::format("nodes {} configs/{} defs", cfgChanged, defChanged);
    for (int s = kSrcLightning; s < kSrcCount; ++s) changed += std::format(", {} {}", kSources[s].id, objChanged[s]);
    SR2_INFO("[{}] pass (settings: re-applied to tracked objects, no scan, {:.2f} ms): tracked [{}] | changed [{}] | "
             "node yields: {} unharvested nodes in loaded zones, {} rescaled ({} -> {} items), {} paced | {}",
             kModule, ms, TrackedSummary(), changed, ns.seen, ns.rescaled, ns.itemsBefore, ns.itemsAfter, ns.paced,
             ParamsSummary(p));
    lastSummaryCounts_ = TrackedSummary();
    return true;
}

std::string SpawnScaling::TrackedSummary() const {
    std::array<size_t, kSrcCount> tracked{};
    for (const auto& kv : objs_) ++tracked[SourceOf(kv.second.kind)];
    std::string counts = std::format("nodes {} configs/{} defs", configs_.size(), defs_.size());
    for (int s = kSrcLightning; s < kSrcCount; ++s) counts += std::format(", {} {}", kSources[s].id, tracked[s]);
    return counts;
}

std::string SpawnScaling::ParamsSummary(const Params& p) const {
    std::string modes;
    for (int s = 0; s < kSrcCount; ++s)
        modes += std::format("{}{}={}", s ? ", " : "", kSources[s].id,
                             !p.apply[s] ? "off" : (p.advanced[s] ? "advanced" : "global"));
    return std::format("spawn rate x{:.2f}, max nodes x{:.2f}, respawn interval x{:.2f}, hard cap {}, ore x{:.2f}, "
                       "roll cap {}, harvest time {} | sources: {}",
                       p.spawnRate, p.maxNodes, p.respawn,
                       p.hardCap > 0 ? std::to_string(p.hardCap) : std::string("auto"), p.ore, p.yieldCap,
                       !paceOk_ ? "n/a" : (p.keepHarvestTime ? "vanilla" : "per item"), modes);
}

Params SpawnScaling::ReadParams() const {
    Params p;
    p.spawnRate = settings::Get(kModule, kSpawnRate);
    p.maxNodes = settings::Get(kModule, kMaxNodes);
    p.respawn = settings::Get(kModule, kRespawn);
    p.hardCap = settings::GetInt(kModule, kHardCap);
    p.ore = settings::Get(kModule, kOreAmount);
    p.yieldCap = settings::GetInt(kModule, kYieldCap);
    p.keepHarvestTime = !settings::Has(kModule, kKeepHarvestTime) || settings::GetBool(kModule, kKeepHarvestTime);
    for (int s = 0; s < kSrcCount; ++s) {
        const std::string a = ApplyKey(s), adv = AdvancedKey(s);
        p.apply[s] = settings::Has(kModule, a) && settings::GetBool(kModule, a);
        p.advanced[s] = settings::Has(kModule, adv) && settings::GetBool(kModule, adv);
    }
    if (!entryOffsetsOk_) p.advanced[kSrcNodes] = false;
    // Defensive clamps (settings already clamp, but never trust 0 / NaN here).
    auto sane = [](double v, double lo, double hi) { return (v >= lo && v <= hi) ? v : (v > hi ? hi : 1.0); };
    p.spawnRate = sane(p.spawnRate, 0.25, 10.0);
    p.maxNodes = sane(p.maxNodes, 0.5, 5.0);
    p.respawn = sane(p.respawn, 0.1, 5.0);
    p.ore = sane(p.ore, 0.5, 10.0);
    if (p.hardCap < 0) p.hardCap = 0;
    if (p.yieldCap < 1) p.yieldCap = 1;
    return p;
}

double SpawnScaling::ResourceMult(const std::string& key) {
    if (key.empty()) return 1.0;
    if (resCacheValid_) {
        auto it = resMultCache_.find(key);
        if (it != resMultCache_.end()) return it->second;
    }
    double m = settings::Has(kModule, key) ? settings::Get(kModule, key) : 1.0;
    if (!(m >= 0.1 && m <= 10.0)) m = 1.0;
    if (resCacheValid_) resMultCache_[key] = m;
    return m;
}

// Amount factor of one resource from one source (weather / drones).
// Advanced mode with an unknown resource (explorer drone default loot entry,
// used for items not in the loot table) falls back to the global multiplier.
double SpawnScaling::SourceFactor(int src, const std::string& resKey, const Params& p) {
    if (!p.apply[src]) return 1.0;
    if (p.advanced[src] && !resKey.empty()) return ResourceMult(resKey);
    return p.ore;
}

bool SpawnScaling::IsNodeDefClass(Il2CppClass* k) const {
    return k && (k == defClass_ || nodeDefClasses_.count(k) != 0);
}

void SpawnScaling::StartCycle(bool active, bool triggered) {
    ++gen_;
    scanParams_ = ReadParams();
    cached_ = scanParams_;
    cachedValid_ = true;
    ++paramsGen_;
    scanWasApplied_ = applied_ || scanStep_ >= 0;
    scanActive_ = active;
    scanTriggered_ = triggered;
    scanStep_ = 0;
    scan_ = ScanStats{};
    zoneScanPending_ = false;
    applied_ = active;  // hooks / light pass follow the new state right away
}

void SpawnScaling::ScanStep() {
    LARGE_INTEGER q0{}, q1{}, qf{};
    QueryPerformanceCounter(&q0);
    const bool active = scanActive_;
    const Params& p = scanParams_;
    resMultCache_.clear();
    resCacheValid_ = true;

    switch (scanStep_) {
        case kStepConfigs:
            for (Il2CppObject* o : unity::FindObjectsOfTypeAll(cfgClass_)) {
                if (!o) continue;
                if (!active) {
                    auto it = configs_.find(o);
                    if (it != configs_.end() && ConfigMatches(o, it->second) && RestoreConfig(o, it->second))
                        ++scan_.cfgChanged;
                    continue;
                }
                bool isNew = false;
                ConfigRec& r = EnsureConfig(o, isNew);
                if (isNew) ++scan_.cfgNew;
                r.seen = gen_;
                if (ApplyConfig(o, r, p)) ++scan_.cfgChanged;
            }
            // Drop records of objects that no longer exist.
            if (active) std::erase_if(configs_, [&](const auto& kv) { return kv.second.seen != gen_; });
            break;
        case kStepDefs:
            for (Il2CppObject* o : unity::FindObjectsOfTypeAll(defClass_)) {
                if (!o) continue;
                if (!active) {
                    auto it = defs_.find(o);
                    if (it != defs_.end() && DefMatches(o, it->second) && RestoreDef(o, it->second))
                        ++scan_.defChanged;
                    continue;
                }
                bool isNew = false;
                DefRec& r = EnsureDef(o, isNew);
                if (isNew) ++scan_.defNew;
                r.seen = gen_;
                if (ApplyDef(o, r, p)) ++scan_.defChanged;
            }
            if (active) std::erase_if(defs_, [&](const auto& kv) { return kv.second.seen != gen_; });
            if (active && !defs_.empty()) nodesSeenEver_ = true;
            break;
        case kStepWeather: ScanWeather(active, p); break;
        case kStepStaticTornado: ScanStaticTornadoes(active, p); break;
        case kStepDrones: ScanDrones(active, p); break;
        default: break;
    }
    resCacheValid_ = false;

    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    const double ms = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 /
                      static_cast<double>(qf.QuadPart > 0 ? qf.QuadPart : 1);
    scan_.totalMs += ms;
    if (ms > scan_.maxMs) scan_.maxMs = ms;
    SR2_DEBUG("[{}] scan step {} took {:.2f} ms", kModule, scanStep_, ms);
    if (++scanStep_ >= kScanSteps) FinishCycle();
}

void SpawnScaling::FinishCycle() {
    scanStep_ = -1;
    const Params& p = scanParams_;
    const auto& objChanged = scan_.objChanged;
    int objChangedTotal = 0;
    for (int c : objChanged) objChangedTotal += c;

    if (!scanActive_) {
        // Everything alive has been restored; forget pointers (objects may be
        // unloaded later and their memory reused).
        configs_.clear();
        defs_.clear();
        objs_.clear();
        nodes_.clear();
        lastSummaryCounts_.clear();
        if (scanWasApplied_ || scan_.cfgChanged || scan_.defChanged || objChangedTotal)
            SR2_INFO("[{}] restored vanilla values: {} director configs, {} node definitions, {} weather/drone objects "
                     "(scan {:.1f} ms over {} frames, max {:.1f} ms/frame)",
                     kModule, scan_.cfgChanged, scan_.defChanged, objChangedTotal, scan_.totalMs,
                     static_cast<int>(kScanSteps), scan_.maxMs);
    } else {
        // One summary line per cycle: what is tracked / changed per source.
        const std::string counts = TrackedSummary();
        std::string changed = std::format("nodes {} configs/{} defs", scan_.cfgChanged, scan_.defChanged);
        for (int s = kSrcLightning; s < kSrcCount; ++s)
            changed += std::format(", {} {}", kSources[s].id, objChanged[s]);
        // Rolled lists of unharvested nodes in loaded zones follow right away.
        const NodeStats ns = NodeSweep(true, "scan", false);
        // Info whenever something happened (enable, values written, node lists
        // rescaled, or the set of tracked objects changed, e.g. a zone loaded);
        // unchanged safety rescans go to Debug to keep the log readable.
        const bool notable = scanTriggered_ || scan_.cfgChanged || scan_.defChanged || objChangedTotal ||
                             ns.rescaled || counts != lastSummaryCounts_;
        lastSummaryCounts_ = counts;
        sr2::log::Logf(notable ? sr2::log::Level::Info : sr2::log::Level::Debug,
                       "[{}] pass{} (scan {:.1f} ms over {} frames, max {:.1f} ms/frame): tracked [{}] | changed [{}] "
                       "(new: {} configs, {} defs) | node yields: {} unharvested nodes in loaded zones, {} rescaled "
                       "({} -> {} items), {} paced | {}",
                       kModule, scanTriggered_ ? " (enable/source on)" : "", scan_.totalMs,
                       static_cast<int>(kScanSteps), scan_.maxMs, counts, changed, scan_.cfgNew, scan_.defNew,
                       ns.seen, ns.rescaled, ns.itemsBefore, ns.itemsAfter, ns.paced, ParamsSummary(p));
    }
    if (sinceFirstPass_ < 0) sinceFirstPass_ = 0;
    FlushPendingResources();
}

// ------------------------------------------------------------ bookkeeping --

SpawnScaling::ConfigRec& SpawnScaling::EnsureConfig(Il2CppObject* cfg, bool& isNew) {
    auto [it, inserted] = configs_.try_emplace(cfg);
    isNew = inserted;
    if (inserted) {
        BaselineConfig(cfg, it->second);
    } else if (!ConfigMatches(cfg, it->second)) {
        // Values changed behind our back (asset reloaded, or the pointer now
        // belongs to a different object): take the current values as vanilla.
        SR2_DEBUG("[{}] config {} changed externally, re-baselining", kModule, it->second.name);
        BaselineConfig(cfg, it->second);
    }
    return it->second;
}

SpawnScaling::DefRec& SpawnScaling::EnsureDef(Il2CppObject* def, bool& isNew) {
    auto [it, inserted] = defs_.try_emplace(def);
    isNew = inserted;
    if (inserted) {
        BaselineDef(def, it->second);
    } else if (!DefMatches(def, it->second)) {
        SR2_DEBUG("[{}] node definition {} changed externally, re-baselining", kModule, it->second.name);
        BaselineDef(def, it->second);
    }
    return it->second;
}

void SpawnScaling::BaselineConfig(Il2CppObject* cfg, ConfigRec& r) {
    r = ConfigRec{};
    r.klass = unity::ClassOf(cfg);
    r.name = unity::GetName(cfg);
    r.origMax = unity::Field<int32_t>(cfg, offMaxActive_);
    r.origInterval = unity::Field<float>(cfg, offInterval_);
    r.wroteMax = r.origMax;
    r.wroteInterval = r.origInterval;
    r.seen = gen_;
}

void SpawnScaling::BaselineDef(Il2CppObject* def, DefRec& r) {
    r = DefRec{};
    r.klass = unity::ClassOf(def);
    if (r.klass) nodeDefClasses_.insert(r.klass);
    r.name = unity::GetName(def);
    r.origMin = unity::Field<int32_t>(def, offMinRes_);
    r.origMax = unity::Field<int32_t>(def, offMaxRes_);
    r.wroteMin = r.origMin;
    r.wroteMax = r.origMax;
    r.resArray = unity::Field<void*>(def, offResources_);
    r.seen = gen_;
    if (entryOffsetsOk_ && r.resArray) {
        const size_t n = unity::ArrayLength(r.resArray);
        for (size_t i = 0; i < n; ++i) {
            void* e = unity::ArrayAt(r.resArray, i);
            if (!e) continue;
            EntryRec er;
            er.entry = e;
            er.origWeight = unity::Field<float>(e, offEntryWeight_);
            er.wroteWeight = er.origWeight;
            auto* type = unity::Field<Il2CppObject*>(e, offEntryType_);
            if (type) {
                er.resName = unity::GetName(type);
                if (!er.resName.empty()) {
                    er.resKey = std::string(kResPrefix) + SanitizeKey(er.resName);
                    NoteResource(er.resKey, er.resName, type, true);
                }
            }
            r.entries.push_back(std::move(er));
        }
    }
}

// Label for a per-resource setting: the localized display name if the game
// can give one cheaply, else the asset name without the "Craft" suffix the
// crafting-resource assets carry ("RadiantOreCraft" -> "Radiant Ore").
std::string SpawnScaling::DisplayLabel(const std::string& rawName, Il2CppObject* type) {
    if (locGetString_ && type) {
        auto* ls = unity::Field<Il2CppObject*>(type, offLocName_);
        if (ls) {
            std::string err;
            Il2CppObject* res = sr2::il2cpp::Resolver::Get().Invoke(locGetString_, ls, nullptr, &err);
            std::string loc = res ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(res)) : std::string();
            // Unity Localization returns an error text instead of throwing when a
            // table entry is missing; only accept short, plausible names.
            const bool bad = loc.empty() || loc.size() > 48 || loc.find("No translation") != std::string::npos ||
                             loc.find('\n') != std::string::npos || loc.find('<') != std::string::npos;
            if (!bad) return loc;
            if (!err.empty()) SR2_DEBUG("[{}] localized name for {} failed: {}", kModule, rawName, err);
        }
    }
    std::string base = rawName;
    constexpr std::string_view kCraft = "Craft";
    if (base.size() > kCraft.size() && base.ends_with(kCraft)) base.resize(base.size() - kCraft.size());
    return PrettyName(base);
}

void SpawnScaling::NoteResource(const std::string& key, const std::string& rawName, Il2CppObject* type, bool ore) {
    if (registeredRes_.count(key)) return;
    auto it = pendingRes_.find(key);
    if (it != pendingRes_.end()) {
        it->second.ore = it->second.ore || ore;
        return;
    }
    PendingRes pr;
    pr.label = DisplayLabel(rawName, type);
    pr.ore = ore;
    pendingRes_.emplace(key, std::move(pr));
}

// Registers pending per-resource settings in one sorted batch: resources seen
// in nodes / lightning / cyclones first (alphabetical), then resources that so
// far only appear in explorer-drone loot tables (plorts, nectar, ...), tagged
// "(drones)". Waits until node definitions have been seen once (they load with
// the game scene, after the weather/drone assets) so the first batch contains
// all ores; falls back after 90 s of scanning.
void SpawnScaling::FlushPendingResources() {
    if (pendingRes_.empty()) return;
    if (!nodesSeenEver_ && sinceFirstPass_ < 90.0f) return;
    struct Item {
        std::string key;
        PendingRes res;
    };
    std::vector<Item> items;
    items.reserve(pendingRes_.size());
    for (auto& [k, v] : pendingRes_) items.push_back({k, v});
    pendingRes_.clear();
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.res.ore != b.res.ore) return a.res.ore;
        return a.res.label < b.res.label;
    });
    for (const Item& it : items) {
        registeredRes_.insert(it.key);
        settings::Def d;
        d.module = kModule;
        d.key = it.key;
        d.label = it.res.ore ? it.res.label : it.res.label + " (drones)";
        d.group = kGroupPerResource;
        d.kind = settings::Kind::Float;
        d.def = 1.0;
        d.min = 0.1;
        d.max = 10.0;
        d.step = 0.1;
        d.unit = "x";
        // Shown only while some source uses the per-resource multipliers (its "advanced" switch on and
        // shown, i.e. that source applied); unregistered sources are skipped by the rule.
        for (int s = 0; s < kSrcCount; ++s) d.anyOf.push_back(AdvancedKey(s));
        d.tooltip = std::format(
            "Amount multiplier for {} on sources set to advanced.{}{}",
            it.res.label,
            it.res.ore ? " On resource nodes it also makes this resource more or less likely."
                       : " Only seen in explorer drone loot so far.",
            kLiveTip);
        settings::Register(d);
        SR2_INFO("[{}] per-resource setting registered: {} ({})", kModule, it.key, d.label);
    }
    // Saved values of the new settings are only visible after Register():
    // re-apply on the next frame.
    dirty_.store(true);
}

bool SpawnScaling::ConfigMatches(Il2CppObject* cfg, const ConfigRec& r) const {
    if (unity::ClassOf(cfg) != r.klass) return false;
    return unity::Field<int32_t>(cfg, offMaxActive_) == r.wroteMax &&
           unity::Field<float>(cfg, offInterval_) == r.wroteInterval;
}

bool SpawnScaling::DefMatches(Il2CppObject* def, const DefRec& r) const {
    if (unity::ClassOf(def) != r.klass) return false;
    if (unity::Field<int32_t>(def, offMinRes_) != r.wroteMin) return false;
    if (unity::Field<int32_t>(def, offMaxRes_) != r.wroteMax) return false;
    void* arr = unity::Field<void*>(def, offResources_);
    if (arr != r.resArray) return false;
    if (entryOffsetsOk_ && arr) {
        size_t idx = 0;
        const size_t n = unity::ArrayLength(arr);
        for (size_t i = 0; i < n; ++i) {
            void* e = unity::ArrayAt(arr, i);
            if (!e) continue;
            if (idx >= r.entries.size() || r.entries[idx].entry != e) return false;
            if (unity::Field<float>(e, offEntryWeight_) != r.entries[idx].wroteWeight) return false;
            ++idx;
        }
        if (idx != r.entries.size()) return false;
    }
    return true;
}

// ------------------------------------------------------------- apply/restore --

bool SpawnScaling::ApplyConfig(Il2CppObject* cfg, ConfigRec& r, const Params& p) {
    const int32_t newMax = ss::ScaleMaxActiveNodes(r.origMax, p.maxNodes, p.hardCap);
    const float newInterval = ss::ScaleIntervalHours(r.origInterval, p.spawnRate, p.respawn);
    if (newMax == r.wroteMax && newInterval == r.wroteInterval) return false;
    unity::Field<int32_t>(cfg, offMaxActive_) = newMax;
    unity::Field<float>(cfg, offInterval_) = newInterval;
    SR2_DEBUG("[{}] config {}: maxActiveNodes {} -> {} (vanilla {}), updateNodesIntervalHours {:.3f} -> {:.3f} "
              "(vanilla {:.3f})",
              kModule, r.name, r.wroteMax, newMax, r.origMax, r.wroteInterval, newInterval, r.origInterval);
    r.wroteMax = newMax;
    r.wroteInterval = newInterval;
    return true;
}

bool SpawnScaling::ApplyDef(Il2CppObject* def, DefRec& r, const Params& p) {
    // Not applied: vanilla. Global mode: count x ore, weights untouched.
    // Advanced mode: weights become w_t * m_t and the count is scaled by
    // sum(w_t * m_t) / sum(w_t), so the expected amount of resource t scales
    // by exactly m_t (all-1.0 leaves everything vanilla).
    const bool applied = p.apply[kSrcNodes];
    const bool advanced = applied && p.advanced[kSrcNodes];
    double f = 1.0;
    std::vector<float> newW(r.entries.size());
    for (size_t i = 0; i < r.entries.size(); ++i) newW[i] = r.entries[i].origWeight;
    if (advanced) {
        double sumW = 0, sumWM = 0;
        for (size_t i = 0; i < r.entries.size(); ++i) {
            const EntryRec& e = r.entries[i];
            const double m = ResourceMult(e.resKey);
            const double w = e.origWeight > 0 ? e.origWeight : 0.0;
            sumW += w;
            sumWM += w * m;
            if (m != 1.0) newW[i] = static_cast<float>(static_cast<double>(e.origWeight) * m);
        }
        f = sumW > 0 ? sumWM / sumW : 1.0;
    } else if (applied) {
        f = p.ore;
    }
    int32_t outMin = 0, outMax = 0;
    ss::ScaleCountPair(r.origMin, r.origMax, f, p.yieldCap, outMin, outMax);
    r.appliedGen = paramsGen_;

    bool changed = false;
    if (outMin != r.wroteMin || outMax != r.wroteMax) {
        unity::Field<int32_t>(def, offMinRes_) = outMin;
        unity::Field<int32_t>(def, offMaxRes_) = outMax;
        SR2_DEBUG("[{}] node {}: resources {}..{} -> {}..{} (vanilla {}..{}, factor {:.3f})", kModule, r.name,
                  r.wroteMin, r.wroteMax, outMin, outMax, r.origMin, r.origMax, f);
        r.wroteMin = outMin;
        r.wroteMax = outMax;
        changed = true;
    }
    for (size_t i = 0; i < r.entries.size(); ++i) {
        EntryRec& e = r.entries[i];
        if (newW[i] == e.wroteWeight) continue;
        unity::Field<float>(e.entry, offEntryWeight_) = newW[i];
        SR2_DEBUG("[{}] node {}: weight[{}] {} {:.3f} -> {:.3f} (vanilla {:.3f})", kModule, r.name, i, e.resName,
                  e.wroteWeight, newW[i], e.origWeight);
        e.wroteWeight = newW[i];
        changed = true;
    }
    return changed;
}

bool SpawnScaling::RestoreConfig(Il2CppObject* cfg, ConfigRec& r) {
    if (r.wroteMax == r.origMax && r.wroteInterval == r.origInterval) return false;
    unity::Field<int32_t>(cfg, offMaxActive_) = r.origMax;
    unity::Field<float>(cfg, offInterval_) = r.origInterval;
    SR2_DEBUG("[{}] config {}: restored maxActiveNodes {}, updateNodesIntervalHours {:.3f}", kModule, r.name,
              r.origMax, r.origInterval);
    r.wroteMax = r.origMax;
    r.wroteInterval = r.origInterval;
    return true;
}

bool SpawnScaling::RestoreDef(Il2CppObject* def, DefRec& r) {
    bool changed = false;
    r.appliedGen = 0;
    if (r.wroteMin != r.origMin || r.wroteMax != r.origMax) {
        unity::Field<int32_t>(def, offMinRes_) = r.origMin;
        unity::Field<int32_t>(def, offMaxRes_) = r.origMax;
        r.wroteMin = r.origMin;
        r.wroteMax = r.origMax;
        changed = true;
    }
    for (EntryRec& e : r.entries) {
        if (e.wroteWeight == e.origWeight) continue;
        unity::Field<float>(e.entry, offEntryWeight_) = e.origWeight;
        e.wroteWeight = e.origWeight;
        changed = true;
    }
    if (changed) SR2_DEBUG("[{}] node {}: restored resources {}..{}", kModule, r.name, r.origMin, r.origMax);
    return changed;
}

// --------------------------------------------------------- weather + drones --

void* SpawnScaling::ListItems(void* list) const {
    if (!list) return nullptr;
    Il2CppClass* k = unity::ClassOf(static_cast<Il2CppObject*>(list));
    if (!k) return nullptr;
    auto it = listItemsOff_.find(k);
    if (it == listItemsOff_.end()) {
        auto off = sr2::il2cpp::Resolver::Get().FieldOffset(k, "_items");
        it = listItemsOff_.emplace(k, off ? *off : 0).first;
    }
    return it->second ? unity::Field<void*>(list, it->second) : nullptr;
}

size_t SpawnScaling::StrideFor(StructLayout& l, void* items) const {
    if (l.stride) return l.stride;
    if (!items || !arrayElementSize_) return 0;
    Il2CppClass* ak = unity::ClassOf(static_cast<Il2CppObject*>(items));
    const int sz = ak ? arrayElementSize_(ak) : 0;
    if (sz <= 0 || static_cast<size_t>(sz) < l.minStride || sz > 0x200) return 0;
    l.stride = static_cast<size_t>(sz);
    return l.stride;
}

// Resources only: plain IdentifiableType assets (SlimeDefinition, GadgetDefinition,
// ToyDefinition, LiquidDefinition are subclasses), not animals or the player.
bool SpawnScaling::IsResourceActor(Il2CppObject* type) const {
    if (!type || unity::ClassOf(type) != identClass_) return false;
    if (unity::Field<uint8_t>(type, offIdentIsAnimal_) != 0) return false;
    if (unity::Field<uint8_t>(type, offIdentIsPlayer_) != 0) return false;
    return true;
}

void SpawnScaling::FillResource(CountPair& c, Il2CppObject* type, bool ore) {
    if (!type) return;
    c.resName = unity::GetName(type);
    if (c.eligible && !c.resName.empty()) {
        c.resKey = std::string(kResPrefix) + SanitizeKey(c.resName);
        NoteResource(c.resKey, c.resName, type, ore);
    }
}

void SpawnScaling::BaselineObj(Il2CppObject* o, ObjKind kind, ObjRec& r) {
    GcRef handle = std::move(r.handle);  // keep (or create) the GC handle across re-baselines
    r = ObjRec{};
    r.handle = std::move(handle);
    if (!r.handle.h) r.handle.set(o);
    r.kind = kind;
    r.klass = unity::ClassOf(o);
    r.name = unity::GetName(o);
    r.seen = gen_;

    auto addPair = [&](void* base, void** baseLoc, int32_t* pMin, int32_t* pMax, bool eligible, Il2CppObject* type) {
        CountPair c;
        c.base = base;
        c.baseLoc = baseLoc;
        c.pMin = pMin;
        c.pMax = pMax;
        c.origMin = c.wroteMin = *pMin;
        c.origMax = c.wroteMax = *pMax;
        c.eligible = eligible;
        FillResource(c, type, SourceOf(kind) != kSrcDrones);
        r.pairs.push_back(std::move(c));
    };
    auto addFloat = [&](float* p) {
        FloatSlot s;
        s.p = p;
        s.orig = s.wrote = *p;
        r.freq.push_back(s);
    };
    auto bindList = [&](size_t listOff) {
        r.list = unity::Field<void*>(o, listOff);
        r.items = ListItems(r.list);
        r.count = (r.list && r.items) ? unity::ListCount(r.list) : 0;
    };

    switch (kind) {
        case ObjKind::LightningPrefab: {
            // List<LightningStrike.ResourceChance> (reference elements).
            bindList(offLsOptions_);
            void** slots = unity::ArrayData<void*>(r.items);
            for (size_t i = 0; i < r.count; ++i) {
                void* e = slots[i];
                if (!e) continue;
                auto* type = unity::Field<Il2CppObject*>(e, offRcResource_);  // null = "nothing" entry
                addPair(e, &slots[i], &unity::Field<int32_t>(e, offRcMin_), &unity::Field<int32_t>(e, offRcMax_),
                        type != nullptr, type);
            }
            break;
        }
        case ObjKind::TornadoActivity:
        case ObjKind::StaticTornado: {
            // List<PeriodicActorSpawner.SpawnOption> (inline struct elements).
            bindList(kind == ObjKind::TornadoActivity ? offTornadoOptions_ : offStOptions_);
            const size_t stride = StrideFor(spawnOptionLayout_, r.items);
            const size_t n = stride ? r.count : 0;  // unknown element size: leave amounts alone
            char* data = unity::ArrayData<char>(r.items);
            for (size_t i = 0; i < n; ++i) {
                char* el = data + i * stride;
                auto* type = *reinterpret_cast<Il2CppObject**>(el + offSoType_);
                addPair(el, nullptr, reinterpret_cast<int32_t*>(el + offSoMin_),
                        reinterpret_cast<int32_t*>(el + offSoMax_), IsResourceActor(type), type);
            }
            if (kind == ObjKind::StaticTornado) {
                addFloat(&unity::Field<float>(o, offStMinDelay_));
                addFloat(&unity::Field<float>(o, offStMaxDelay_));
                r.freqFloor = kMinTornadoDelayHours;
            }
            break;
        }
        case ObjKind::DroneConfig: {
            // List<ExplorerDroneConfig.LootTableEntry> (inline structs) whose
            // OutputQuantity is a reference to a Util.RangeInt {Min, Max}.
            bindList(offLootTable_);
            const size_t stride = StrideFor(lootLayout_, r.items);
            const size_t n = stride ? r.count : 0;  // unknown element size: table entries left alone
            char* data = unity::ArrayData<char>(r.items);
            auto addLoot = [&](char* entry) {
                auto* item = *reinterpret_cast<Il2CppObject**>(entry + offLteItem_);
                void** loc = reinterpret_cast<void**>(entry + offLteOutput_);
                void* ri = *loc;
                if (!ri) return;
                addPair(ri, loc, &unity::Field<int32_t>(ri, offRiMin_), &unity::Field<int32_t>(ri, offRiMax_), true,
                        item);
            };
            for (size_t i = 0; i < n; ++i) addLoot(data + i * stride);
            addLoot(reinterpret_cast<char*>(o) + offDefaultLoot_);  // fallback entry for items not in the table
            break;
        }
        case ObjKind::LightningActivity:
            break;
    }
    if (kind == ObjKind::LightningActivity || kind == ObjKind::TornadoActivity) {
        r.range = unity::Field<void*>(o, offActRange_);
        if (r.range) {
            addFloat(&unity::Field<float>(r.range, offRangeMin_));
            addFloat(&unity::Field<float>(r.range, offRangeMax_));
        }
        r.freqFloor = kMinSecondsBetweenWeatherSpawns;
    }
}

bool SpawnScaling::ObjMatches(Il2CppObject* o, const ObjRec& r) const {
    if (unity::ClassOf(o) != r.klass) return false;
    size_t listOff = 0;
    switch (r.kind) {
        case ObjKind::LightningPrefab: listOff = offLsOptions_; break;
        case ObjKind::TornadoActivity: listOff = offTornadoOptions_; break;
        case ObjKind::StaticTornado: listOff = offStOptions_; break;
        case ObjKind::DroneConfig: listOff = offLootTable_; break;
        case ObjKind::LightningActivity: break;
    }
    if (listOff) {
        void* list = unity::Field<void*>(o, listOff);
        if (list != r.list) return false;
        if (list) {
            void* items = ListItems(list);
            if (items != r.items) return false;
            if ((items ? unity::ListCount(list) : 0) != r.count) return false;
        }
    }
    if (r.kind == ObjKind::LightningActivity || r.kind == ObjKind::TornadoActivity) {
        if (unity::Field<void*>(o, offActRange_) != r.range) return false;
    }
    for (const CountPair& c : r.pairs) {
        if (c.baseLoc && *c.baseLoc != c.base) return false;
        if (*c.pMin != c.wroteMin || *c.pMax != c.wroteMax) return false;
    }
    for (const FloatSlot& s : r.freq)
        if (*s.p != s.wrote) return false;
    return true;
}

bool SpawnScaling::ApplyObj(ObjRec& r, const Params& p) {
    const int src = SourceOf(r.kind);
    bool changed = false;
    r.appliedGen = paramsGen_;
    for (size_t i = 0; i < r.pairs.size(); ++i) {
        CountPair& c = r.pairs[i];
        const double f = c.eligible ? SourceFactor(src, c.resKey, p) : 1.0;
        int32_t nMin = 0, nMax = 0;
        ss::ScaleCountPair(c.origMin, c.origMax, f, p.yieldCap, nMin, nMax);
        if (nMin == c.wroteMin && nMax == c.wroteMax) continue;
        *c.pMin = nMin;
        *c.pMax = nMax;
        SR2_DEBUG("[{}] {} {}: option[{}] {} amount {}..{} -> {}..{} (vanilla {}..{}, factor {:.3f})", kModule,
                  KindName(r.kind), r.name, i, c.resName.empty() ? "(any/none)" : c.resName, c.wroteMin, c.wroteMax,
                  nMin, nMax, c.origMin, c.origMax, f);
        c.wroteMin = nMin;
        c.wroteMax = nMax;
        changed = true;
    }
    const double rate = p.apply[src] ? p.spawnRate : 1.0;
    for (FloatSlot& s : r.freq) {
        const float v = ss::ScaleDuration(s.orig, rate, r.freqFloor);
        if (v == s.wrote) continue;
        *s.p = v;
        SR2_DEBUG("[{}] {} {}: time between spawns {:.3f} -> {:.3f} (vanilla {:.3f})", kModule, KindName(r.kind),
                  r.name, s.wrote, v, s.orig);
        s.wrote = v;
        changed = true;
    }
    return changed;
}

bool SpawnScaling::RestoreObj(ObjRec& r) {
    bool changed = false;
    for (CountPair& c : r.pairs) {
        if (c.wroteMin == c.origMin && c.wroteMax == c.origMax) continue;
        *c.pMin = c.origMin;
        *c.pMax = c.origMax;
        c.wroteMin = c.origMin;
        c.wroteMax = c.origMax;
        changed = true;
    }
    for (FloatSlot& s : r.freq) {
        if (s.wrote == s.orig) continue;
        *s.p = s.orig;
        s.wrote = s.orig;
        changed = true;
    }
    if (changed) SR2_DEBUG("[{}] {} {}: restored vanilla", kModule, KindName(r.kind), r.name);
    return changed;
}

bool SpawnScaling::HandleObj(Il2CppObject* o, ObjKind kind, bool active, bool allowCreate, const Params& p) {
    auto it = objs_.find(o);
    if (!active) {
        return it != objs_.end() && ObjMatches(o, it->second) && RestoreObj(it->second);
    }
    if (it == objs_.end()) {
        if (!allowCreate || !p.apply[SourceOf(kind)]) return false;
        it = objs_.try_emplace(o).first;
        BaselineObj(o, kind, it->second);
        SR2_DEBUG("[{}] tracking {} {} ({} amount entries, {} timing values)", kModule, KindName(kind),
                  it->second.name, it->second.pairs.size(), it->second.freq.size());
    } else if (it->second.seen == gen_ && it->second.appliedGen == paramsGen_) {
        return false;  // already handled this pass (prefab shared by several activities)
    } else if (!ObjMatches(o, it->second)) {
        SR2_DEBUG("[{}] {} {} changed externally, re-baselining", kModule, KindName(kind), it->second.name);
        BaselineObj(o, kind, it->second);
    }
    it->second.seen = gen_;
    return ApplyObj(it->second, p);
}

bool SpawnScaling::WantWeather(bool active, const Params& p) const {
    return weatherOk_ && (!objs_.empty() || (active && (p.apply[kSrcLightning] || p.apply[kSrcCyclone])));
}

bool SpawnScaling::WantDrones(bool active, const Params& p) const {
    return dronesOk_ && (!objs_.empty() || (active && p.apply[kSrcDrones]));
}

// Drop records (of the kinds one scan step covers) of objects that no longer
// exist, and of sources that are switched off (their values were just
// written back to vanilla).
void SpawnScaling::EraseUnseenObjs(std::initializer_list<ObjKind> kinds, const Params& p) {
    std::erase_if(objs_, [&](const auto& kv) {
        bool mine = false;
        for (ObjKind k : kinds) mine = mine || kv.second.kind == k;
        return mine && (kv.second.seen != gen_ || !p.apply[SourceOf(kv.second.kind)]);
    });
}

void SpawnScaling::ScanWeather(bool active, const Params& p) {
    auto& changed = scan_.objChanged;
    if (WantWeather(active, p)) {
        // Weather activities are ScriptableObject assets. Tornado activities
        // carry the cyclone SpawnOptions; plain temporary-object activities
        // whose prefab has a LightningStrike component are the lightning
        // spawners - the prefab component itself holds the lightning options.
        for (Il2CppObject* act : unity::FindObjectsOfTypeAll(tempActClass_)) {
            if (!act) continue;
            Il2CppClass* k = unity::ClassOf(act);
            if (k == tornadoActClass_) {
                if (HandleObj(act, ObjKind::TornadoActivity, active, true, p)) ++changed[kSrcCyclone];
                continue;
            }
            if (k != tempActClass_) continue;  // other subclasses: not weather resources
            auto* prefab = unity::Field<Il2CppObject*>(act, offActPrefab_);
            if (!prefab) continue;
            Il2CppObject* strike = unity::GetComponent(prefab, lightningClass_);
            if (!strike) continue;
            if (HandleObj(act, ObjKind::LightningActivity, active, true, p)) ++changed[kSrcLightning];
            if (HandleObj(strike, ObjKind::LightningPrefab, active, true, p)) ++changed[kSrcLightning];
        }
    }
    if (active) EraseUnseenObjs({ObjKind::LightningActivity, ObjKind::LightningPrefab, ObjKind::TornadoActivity}, p);
}

void SpawnScaling::ScanStaticTornadoes(bool active, const Params& p) {
    if (WantWeather(active, p)) {
        // Scene-placed cyclone spawners. New records only for objects active
        // in a scene hierarchy (excludes prefab assets, whose runtime
        // instances would otherwise be double-patched); already-tracked ones
        // stay tracked while they exist, active or not.
        for (Il2CppObject* st : unity::FindObjectsOfTypeAll(staticTornadoClass_)) {
            if (!st) continue;
            const bool tracked = objs_.count(st) != 0;
            const bool allowCreate = tracked || unity::ActiveInHierarchy(unity::GameObjectOf(st));
            if (HandleObj(st, ObjKind::StaticTornado, active, allowCreate, p)) ++scan_.objChanged[kSrcCyclone];
        }
    }
    if (active) EraseUnseenObjs({ObjKind::StaticTornado}, p);
}

void SpawnScaling::ScanDrones(bool active, const Params& p) {
    if (WantDrones(active, p)) {
        for (Il2CppObject* cfg : unity::FindObjectsOfTypeAll(droneCfgClass_)) {
            if (cfg && HandleObj(cfg, ObjKind::DroneConfig, active, true, p)) ++scan_.objChanged[kSrcDrones];
        }
    }
    if (active) EraseUnseenObjs({ObjKind::DroneConfig}, p);
}

// --------------------------------------------------------- hook callbacks --

void SpawnScaling::TouchDirector(Il2CppObject* director) {
    if (!enabled_.load() || !applied_ || !cachedValid_) return;
    auto* cfg = unity::Field<Il2CppObject*>(director, offDirConfig_);
    if (!cfg) return;
    bool isNew = false;
    ConfigRec& r = EnsureConfig(cfg, isNew);
    r.seen = gen_;
    ApplyConfig(cfg, r, cached_);
    if (isNew) SR2_DEBUG("[{}] director Awake: patched config {} on first touch", kModule, r.name);
}

void SpawnScaling::TouchWeather(Il2CppObject* obj, bool staticSpawner) {
    if (!enabled_.load() || !applied_ || !cachedValid_ || !weatherOk_) return;
    Il2CppClass* k = unity::ClassOf(obj);
    if (staticSpawner) {
        // OnEnable only runs for scene instances (never for prefab assets).
        if (k == staticTornadoClass_) HandleObj(obj, ObjKind::StaticTornado, true, true, cached_);
        return;
    }
    if (k == tornadoActClass_) {
        HandleObj(obj, ObjKind::TornadoActivity, true, true, cached_);
        return;
    }
    if (k != tempActClass_) return;
    auto* prefab = unity::Field<Il2CppObject*>(obj, offActPrefab_);
    Il2CppObject* strike = prefab ? unity::GetComponent(prefab, lightningClass_) : nullptr;
    if (!strike) return;
    HandleObj(obj, ObjKind::LightningActivity, true, true, cached_);
    HandleObj(strike, ObjKind::LightningPrefab, true, true, cached_);
}

void SpawnScaling::TouchDefinition(Il2CppObject* def) {
    if (!enabled_.load() || !applied_ || !cachedValid_) return;
    Il2CppClass* k = unity::ClassOf(def);
    if (!IsNodeDefClass(k)) return;  // ResourceGrowerDefinition etc. are not touched
    auto it = defs_.find(def);
    if (it != defs_.end() && it->second.appliedGen == paramsGen_ && DefMatches(def, it->second))
        return;  // already holds the current settings (fast path)
    bool isNew = false;
    DefRec& r = EnsureDef(def, isNew);
    r.seen = gen_;
    ApplyDef(def, r, cached_);
    if (isNew) SR2_DEBUG("[{}] GetResourcesToSpawn: patched node {} on first touch", kModule, r.name);
}

// ------------------------------------------------------- rolled node lists --
//
// ResourceNodeSpawner.SpawnNode stores GetResourcesToSpawn()'s list in
// ResourceNodeSpawnerModel.resourcesToSpawn once; the harvest coroutine
// (ResourceNode.SpawnResourcesAsync) instantiates exactly that list. A list is
// only rewritten while its node is READY (nobody enumerates it then) or right
// before ResourceNode.SpawnResources starts the coroutine.

const char* SpawnScaling::OriginName(NodeOrigin o) {
    switch (o) {
        case NodeOrigin::Rolled: return "rolled this session";
        case NodeOrigin::Vanilla: return "vanilla roll (save / module off)";
        case NodeOrigin::PreScaled: return "already scaled earlier (outside vanilla range)";
        case NodeOrigin::Unknown: break;
    }
    return "unknown";
}

bool SpawnScaling::IsIdentType(Il2CppClass* k) {
    if (!k) return false;
    auto it = identClassCache_.find(k);
    if (it != identClassCache_.end()) return it->second;
    auto& A = sr2::il2cpp::api();
    bool ok = false;
    if (A.il2cpp_class_is_assignable_from) {
        ok = A.il2cpp_class_is_assignable_from(nodeIdentClass_, k);
    } else if (A.il2cpp_class_get_parent) {
        for (Il2CppClass* c = k; c && !ok; c = A.il2cpp_class_get_parent(c)) ok = c == nodeIdentClass_;
    } else {
        ok = k == nodeIdentClass_;
    }
    identClassCache_.emplace(k, ok);
    return ok;
}

bool SpawnScaling::ReadRolledList(void* list, std::vector<Il2CppObject*>& out) {
    out.clear();
    if (!list || unity::ClassOf(static_cast<Il2CppObject*>(list)) != rolledListClass_) return false;
    const int32_t size = unity::Field<int32_t>(list, offListSize_);
    if (size < 0 || static_cast<size_t>(size) > kMaxRolledListLen) return false;
    if (size == 0) return true;
    void* items = unity::Field<void*>(list, offListItems_);
    if (!items || unity::ArrayLength(items) < static_cast<size_t>(size)) return false;
    Il2CppObject** data = unity::ArrayData<Il2CppObject*>(items);
    out.reserve(static_cast<size_t>(size));
    for (int32_t i = 0; i < size; ++i) {
        Il2CppObject* e = data[i];
        if (!e || !IsIdentType(unity::ClassOf(e))) {
            out.clear();
            return false;
        }
        out.push_back(e);
    }
    return true;
}

// Writes `items` into the List<IdentifiableType> in place (same list object,
// so the model / save keep pointing at it). Growth goes through the managed
// set_Capacity; element stores use the GC write barrier when exported.
bool SpawnScaling::WriteRolledList(void* list, const std::vector<Il2CppObject*>& items) {
    const size_t n = items.size();
    if (n > kMaxRolledListLen) return false;
    const int32_t oldSize = unity::Field<int32_t>(list, offListSize_);
    void* arr = unity::Field<void*>(list, offListItems_);
    if (!arr || unity::ArrayLength(arr) < n) {
        int32_t cap = static_cast<int32_t>(n);
        void* args[1] = {&cap};
        std::string err;
        sr2::il2cpp::Resolver::Get().Invoke(listSetCapacity_, list, args, &err);
        if (!err.empty()) {
            SR2_WARN("[{}] List.set_Capacity({}) failed: {}", kModule, cap, err);
            return false;
        }
        arr = unity::Field<void*>(list, offListItems_);
        if (!arr || unity::ArrayLength(arr) < n) return false;
    }
    Il2CppObject** data = unity::ArrayData<Il2CppObject*>(arr);
    auto* arrObj = static_cast<Il2CppObject*>(arr);
    const size_t len = unity::ArrayLength(arr);
    auto store = [&](size_t i, Il2CppObject* v) {
        data[i] = v;
        if (wbarrier_) wbarrier_(arrObj, reinterpret_cast<void**>(&data[i]), v);
    };
    for (size_t i = 0; i < n; ++i) store(i, items[i]);
    for (size_t i = n; i < static_cast<size_t>(oldSize > 0 ? oldSize : 0) && i < len; ++i) store(i, nullptr);
    unity::Field<int32_t>(list, offListSize_) = static_cast<int32_t>(n);
    ++unity::Field<int32_t>(list, offListVersion_);
    return true;
}

const std::pair<std::string, std::string>& SpawnScaling::TypeNames(Il2CppObject* type) {
    auto it = typeNames_.find(type);
    if (it != typeNames_.end()) return it->second;
    std::string raw = unity::GetName(type);
    std::string key = raw.empty() ? std::string() : std::string(kResPrefix) + SanitizeKey(raw);
    return typeNames_.emplace(type, std::make_pair(std::move(key), std::move(raw))).first->second;
}

// Per-type multiplier a rolled node list should carry now (the same rule the
// definition patch uses: global ore, per-resource in advanced mode, 1 if off).
double SpawnScaling::NodeTypeMult(Il2CppObject* type, const Params& p, bool active) {
    if (!active || !p.apply[kSrcNodes]) return 1.0;
    if (p.advanced[kSrcNodes]) return ResourceMult(TypeNames(type).first);
    return p.ore;
}

void SpawnScaling::DefRanges(Il2CppObject* def, int32_t& vMin, int32_t& vMax, int32_t& pMin, int32_t& pMax,
                             std::string& name) {
    vMin = vMax = pMin = pMax = 0;
    name = "?";
    if (!def || !IsNodeDefClass(unity::ClassOf(def))) return;
    pMin = unity::Field<int32_t>(def, offMinRes_);
    pMax = unity::Field<int32_t>(def, offMaxRes_);
    auto it = defs_.find(def);
    if (it != defs_.end() && it->second.klass == unity::ClassOf(def)) {
        vMin = it->second.origMin;
        vMax = it->second.origMax;
        name = it->second.name;
    } else {  // never patched by us: the current values are vanilla
        vMin = pMin;
        vMax = pMax;
        name = unity::GetName(def);
    }
}

bool SpawnScaling::DefPatchedNow(Il2CppObject* def) {
    auto it = defs_.find(def);
    return it != defs_.end() && it->second.appliedGen == paramsGen_ && DefMatches(def, it->second);
}

void SpawnScaling::BaselineNode(NodeRec& r, std::vector<Il2CppObject*>&& items, Il2CppObject* def, NodeOrigin origin,
                                const Params& p, bool active) {
    r = NodeRec{};
    r.base = std::move(items);
    r.wrote = r.base;
    int32_t vMin = 0, vMax = 0, pMin = 0, pMax = 0;
    DefRanges(def, vMin, vMax, pMin, pMax, r.defName);
    if (origin == NodeOrigin::Unknown) {
        // Unknown provenance (loaded from the save, or rolled before we were
        // on): vanilla only if the length fits the definition's vanilla range,
        // otherwise it already carries a multiplier and is taken as matching
        // the current settings (never compounded).
        const auto n = static_cast<int32_t>(r.base.size());
        origin = (vMax > 0 && n >= vMin && n <= vMax) ? NodeOrigin::Vanilla : NodeOrigin::PreScaled;
    }
    r.origin = origin;
    for (Il2CppObject* t : r.base) {
        auto it = std::find_if(r.types.begin(), r.types.end(), [&](const NodeTypeCount& c) { return c.type == t; });
        if (it == r.types.end()) {
            NodeTypeCount c;
            c.type = t;
            // Rolled: the definition held the current settings during the roll
            // (OnRolled passes Vanilla instead when it did not).
            c.mBase = origin == NodeOrigin::Vanilla ? 1.0 : NodeTypeMult(t, p, active);
            r.types.push_back(c);
            it = r.types.end() - 1;
        }
        ++it->count;
    }
    r.paceRatio = PaceRatioOf(r);
}

// Vanilla-equivalent amount of the list / items it holds (<= 1). Every write
// is base x mNow / mBase, so the vanilla-equivalent amount is sum(count / mBase).
double SpawnScaling::PaceRatioOf(const NodeRec& r) {
    if (r.wrote.empty()) return 1.0;
    double vanilla = 0;
    for (const NodeTypeCount& c : r.types) vanilla += static_cast<double>(c.count) / (c.mBase > 0 ? c.mBase : 1.0);
    const double ratio = vanilla / static_cast<double>(r.wrote.size());
    return (ratio > 0 && ratio < 1.0) ? ratio : 1.0;
}

// Target list: per resource type count_t x (m_now / m_base), total rounded
// (>= 1, roll cap only limits increases), split by largest remainder. Base
// items are kept in their order (MinimumAmount picks come first in a roll);
// extra copies are appended. m_now == m_base everywhere -> the base list.
std::vector<Il2CppObject*> SpawnScaling::NodeTarget(const NodeRec& r, const Params& p, bool active) {
    const size_t baseN = r.base.size();
    if (baseN == 0 || r.types.empty()) return r.base;
    std::vector<double> want(r.types.size());
    double sum = 0;
    bool same = true;
    for (size_t i = 0; i < r.types.size(); ++i) {
        const double mNow = NodeTypeMult(r.types[i].type, p, active);
        const double mBase = r.types[i].mBase > 0 ? r.types[i].mBase : 1.0;
        const double ratio = mNow / mBase;
        if (ratio != 1.0) same = false;
        want[i] = static_cast<double>(r.types[i].count) * ratio;
        sum += want[i];
    }
    if (same) return r.base;
    const int64_t total = ss::ScaledListTotal(sum, baseN, p.yieldCap, kMaxRolledListLen);
    const std::vector<int32_t> alloc = ss::SplitLargestRemainder(want, total);

    std::vector<Il2CppObject*> out;
    out.reserve(static_cast<size_t>(total));
    std::vector<int32_t> kept(r.types.size(), 0);
    auto indexOf = [&](Il2CppObject* t) -> size_t {
        for (size_t i = 0; i < r.types.size(); ++i)
            if (r.types[i].type == t) return i;
        return 0;
    };
    for (Il2CppObject* t : r.base) {
        const size_t i = indexOf(t);
        if (kept[i] < alloc[i]) {
            out.push_back(t);
            ++kept[i];
        }
    }
    for (size_t i = 0; i < r.types.size(); ++i)
        for (; kept[i] < alloc[i]; ++kept[i]) out.push_back(r.types[i].type);
    return out;
}

bool SpawnScaling::ProcessNodeList(void* list, Il2CppObject* def, bool active, const Params& p, NodeStats& st,
                                   NodeRec** outRec, int* outBefore) {
    std::vector<Il2CppObject*> cur;
    if (!ReadRolledList(list, cur)) return false;
    ++st.seen;
    if (outBefore) *outBefore = static_cast<int>(cur.size());
    auto it = nodes_.find(list);
    if (it != nodes_.end() && it->second.wrote != cur && !cur.empty() && cur.size() < it->second.wrote.size() &&
        std::equal(cur.begin(), cur.end(), it->second.wrote.end() - static_cast<std::ptrdiff_t>(cur.size()))) {
        // Partly harvested: the vacuum pops list[0] one by one and the node goes
        // back to READY when the vac is released. What is left is the tail of
        // what we wrote: keep it as it is (rescaling the remainder would make a
        // node refill, e.g. 3 left of 30 looks like a vanilla 2..4 roll).
        NodeRec& r = it->second;
        if (!r.partial && partialLogs_ < kNodeLogsPerZone) {
            ++partialLogs_;
            SR2_INFO("[{}] node partly harvested: {} has {} of {} item(s) left (vac released) - left as it is", kModule,
                     r.defName, cur.size(), r.wrote.size());
        }
        r.partial = true;
        r.wrote = std::move(cur);
        r.sweep = sweepId_;
        if (outRec) *outRec = &r;
        return false;
    }
    if (it != nodes_.end() && it->second.wrote != cur) {
        // Changed by the game since we last wrote it (or the address now
        // belongs to another list): start over from what it holds now.
        SR2_DEBUG("[{}] rolled list of {} changed externally ({} -> {} items), re-baselining", kModule,
                  it->second.defName, it->second.wrote.size(), cur.size());
        nodes_.erase(it);
        it = nodes_.end();
    }
    if (it == nodes_.end()) {
        if (!active) return false;  // never touched by us: nothing to undo
        it = nodes_.try_emplace(list).first;
        BaselineNode(it->second, std::vector<Il2CppObject*>(cur), def, NodeOrigin::Unknown, p, active);
    }
    NodeRec& r = it->second;
    r.sweep = sweepId_;
    if (outRec) *outRec = &r;
    if (r.partial) return false;                              // partly harvested: never rescaled
    if (active && r.appliedGen == paramsGen_) return false;  // already at the current settings
    std::vector<Il2CppObject*> target = NodeTarget(r, p, active);
    r.appliedGen = active ? paramsGen_ : 0;
    if (target == cur) return false;
    if (!WriteRolledList(list, target)) return false;
    SR2_DEBUG("[{}] node {} ({}): rolled list {} -> {} items (base {})", kModule, r.defName, OriginName(r.origin),
              cur.size(), target.size(), r.base.size());
    st.itemsBefore += static_cast<int>(cur.size());
    st.itemsAfter += static_cast<int>(target.size());
    ++st.rescaled;
    r.wrote = std::move(target);
    r.paceRatio = PaceRatioOf(r);
    return true;
}

SpawnScaling::NodeStats SpawnScaling::NodeSweep(bool active, const char* reason, bool log) {
    NodeStats st;
    if (!nodeListsOk_ || !directorsFn_ || !cachedValid_) return st;
    LARGE_INTEGER q0{}, q1{}, qf{};
    QueryPerformanceCounter(&q0);
    Il2CppObject* dirs = nullptr;
    std::string err;
    if (!sr2::core::Try([&] { dirs = directorsFn_(directorsMi_); }, &err)) return st;
    ++sweepId_;
    const Params& p = cached_;
    resMultCache_.clear();
    resCacheValid_ = true;
    const size_t nd = unity::ListCount(dirs);
    for (size_t i = 0; i < nd; ++i) {
        auto* dir = static_cast<Il2CppObject*>(unity::ListAt(dirs, i));
        if (!dir || !unity::IsAlive(dir)) continue;
        void* spawners = unity::Field<void*>(dir, offDirSpawners_);
        const size_t ns = unity::ListCount(spawners);
        for (size_t j = 0; j < ns; ++j) {
            auto* sp = static_cast<Il2CppObject*>(unity::ListAt(spawners, j));
            if (!sp || !unity::IsAlive(sp)) continue;
            auto* model = unity::Field<Il2CppObject*>(sp, offSpawnerModel_);
            if (!model || unity::ClassOf(model) != modelClass_) continue;
            // The spawned ResourceNode instance (harvest pacing), validated.
            Il2CppObject* node = paceOk_ ? unity::Field<Il2CppObject*>(sp, offSpawnerNode_) : nullptr;
            if (node && !IsNodeObject(node, model)) node = nullptr;
            if (unity::Field<int32_t>(model, offModelState_) != nodeStateReady_) {
                // Harvesting / harvested / despawning: the list is not ours to
                // touch now. Disable still gives the node its vanilla timing back.
                if (!active && node) ApplyPace(node, 1.0, std::string());
                continue;
            }
            void* list = unity::Field<void*>(model, offModelList_);
            if (!list) continue;
            NodeRec* rec = nullptr;
            ProcessNodeList(list, unity::Field<Il2CppObject*>(model, offModelDef_), active, p, st, &rec);
            if (node) {
                const double ratio = (active && p.keepHarvestTime && rec) ? rec->paceRatio : 1.0;
                if (ApplyPace(node, ratio, rec ? rec->defName : std::string())) ++st.paced;
            }
        }
    }
    resCacheValid_ = false;
    if (nodes_.size() > kMaxNodeRecords)
        std::erase_if(nodes_, [&](const auto& kv) { return kv.second.sweep != sweepId_; });
    if (paces_.size() > kMaxPaceRecords)  // map bookkeeping only: unseen instances are never dereferenced
        std::erase_if(paces_, [&](const auto& kv) { return kv.second.sweep != sweepId_; });
    if (!active) sessions_.clear();
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    st.ms = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 /
            static_cast<double>(qf.QuadPart > 0 ? qf.QuadPart : 1);
    if (log && st.rescaled)
        SR2_INFO("[{}] node yields ({}): rescaled {} of {} unharvested nodes in loaded zones ({} -> {} items, "
                 "{:.2f} ms) | harvest pacing on {} node(s)",
                 kModule, reason, st.rescaled, st.seen, st.itemsBefore, st.itemsAfter, st.ms, st.paced);
    else
        SR2_DEBUG("[{}] node sweep ({}): {} READY nodes, {} rescaled, {} paced, {:.2f} ms", kModule, reason, st.seen,
                  st.rescaled, st.paced, st.ms);
    return st;
}

std::string SpawnScaling::TypeSummary(const std::vector<Il2CppObject*>& items) {
    std::vector<std::pair<Il2CppObject*, int>> agg;
    for (Il2CppObject* t : items) {
        auto it = std::find_if(agg.begin(), agg.end(), [&](const auto& a) { return a.first == t; });
        if (it == agg.end())
            agg.emplace_back(t, 1);
        else
            ++it->second;
    }
    std::string s;
    for (const auto& [t, n] : agg) {
        if (!s.empty()) s += ", ";
        s += std::format("{} x{}", TypeNames(t).second, n);
    }
    return s.empty() ? std::string("empty") : s;
}

// After GetResourcesToSpawn (node definitions only): remember the fresh list
// with the multipliers its definition carried, scale it now if the definition
// was not patched, and log the roll (first kNodeLogsPerZone per zone).
void SpawnScaling::OnRolled(Il2CppObject* def, Il2CppObject* list, bool forceMax) {
    if (!enabled_.load() || !applied_ || !cachedValid_) return;
    if (!IsNodeDefClass(unity::ClassOf(def))) return;  // growers etc.
    const bool patched = DefPatchedNow(def);
    std::vector<Il2CppObject*> items;
    bool tracked = false;
    if (nodeListsOk_ && ReadRolledList(list, items)) {
        NodeRec& r = nodes_[list];
        BaselineNode(r, std::move(items), def, patched ? NodeOrigin::Rolled : NodeOrigin::Vanilla, cached_, true);
        r.sweep = sweepId_;
        NodeStats st;
        ProcessNodeList(list, def, true, cached_, st);  // no-op when the roll already used the current settings
        tracked = true;
    }
    if (spawnLogs_ >= kNodeLogsPerZone) return;
    ++spawnLogs_;
    int32_t vMin = 0, vMax = 0, pMin = 0, pMax = 0;
    std::string name;
    DefRanges(def, vMin, vMax, pMin, pMax, name);
    std::vector<Il2CppObject*> now;
    if (nodeListsOk_) ReadRolledList(list, now);
    SR2_INFO("[{}] node spawned: {} rolled {} item(s){} ({}) | vanilla {}..{}, patched {}..{} (ore x{:.2f}, nodes={}) "
             "| {}{}",
             kModule, name, nodeListsOk_ ? std::to_string(now.size()) : std::string("?"), forceMax ? " [max count]" : "",
             nodeListsOk_ ? TypeSummary(now) : std::string("list not readable"), vMin, vMax, pMin, pMax, cached_.ore,
             !cached_.apply[kSrcNodes] ? "off" : (cached_.advanced[kSrcNodes] ? "advanced" : "global"),
             patched ? "definition held the current settings" : "definition was NOT patched -> list scaled after roll",
             tracked ? "" : " (list not tracked)");
    if (spawnLogs_ == kNodeLogsPerZone)
        SR2_INFO("[{}] (further 'node spawned' lines suppressed until the next zone change)", kModule);
}

// Before ResourceNode.SpawnResources: bring the harvested node's rolled list to
// the current settings (the coroutine started by the original spawns exactly
// that list), and log what the player gets (first kNodeLogsPerZone per zone).
void SpawnScaling::OnHarvest(Il2CppObject* node) {
    if (!enabled_.load() || !applied_ || !cachedValid_ || !nodeListsOk_) return;
    auto* model = unity::Field<Il2CppObject*>(node, offNodeModel_);
    if (!model || unity::ClassOf(model) != modelClass_) return;
    void* list = unity::Field<void*>(model, offModelList_);
    auto* def = unity::Field<Il2CppObject*>(model, offModelDef_);
    NodeStats st;
    NodeRec* rec = nullptr;
    int before = -1;
    resMultCache_.clear();
    resCacheValid_ = true;
    const bool rescaled = list && ProcessNodeList(list, def, true, cached_, st, &rec, &before);
    resCacheValid_ = false;
    if (harvestLogs_ >= kNodeLogsPerZone) return;
    ++harvestLogs_;
    int32_t vMin = 0, vMax = 0, pMin = 0, pMax = 0;
    std::string name;
    DefRanges(def, vMin, vMax, pMin, pMax, name);
    std::vector<Il2CppObject*> now;
    const bool readable = list && ReadRolledList(list, now);
    SR2_INFO("[{}] node harvested: {} yields {} item(s) ({}) | stored list had {}{} [{}] | vanilla {}..{}, patched "
             "{}..{} (ore x{:.2f}, nodes={})",
             kModule, name, readable ? std::to_string(now.size()) : std::string("?"),
             readable ? TypeSummary(now) : std::string("list not readable"), before,
             rescaled ? ", rescaled just now" : "", rec ? OriginName(rec->origin) : "not tracked", vMin, vMax, pMin,
             pMax, cached_.ore,
             !cached_.apply[kSrcNodes] ? "off" : (cached_.advanced[kSrcNodes] ? "advanced" : "global"));
    if (harvestLogs_ == kNodeLogsPerZone)
        SR2_INFO("[{}] (further 'node harvested' lines suppressed until the next zone change)", kModule);
}


// ---------------------------------------------------------- harvest pacing --
//
// Vacuum harvest (1.3.0, disassembly): READY + vac on the node -> HARVESTING,
// _harvestAtTime = now + _timeToHarvest. While HARVESTING and the vac stays on
// it, UpdateForState calls SpawnSingleResource whenever now >= _harvestAtTime:
// it pops list[0] (RemoveAt(0)), sets _harvestAtTime = now + _timeToHarvest and
// goes EMPTY when the list is empty. Vac released -> READY with the rest.
// _breakMode nodes instead start SpawnResourcesAsync after one _timeToHarvest
// wait, one item per WaitForSeconds(_resourceSpawnDelaySeconds) (read live each
// step). So the harvest time of a node is count x per-item time, and pacing
// scales the per-item time of THIS node instance by vanilla / scaled count.

bool SpawnScaling::IsNodeObject(Il2CppObject* node, Il2CppObject* model) {
    if (!node || !nodeClass_) return false;
    Il2CppClass* k = unity::ClassOf(node);
    if (!k) return false;
    auto it = nodeClassCache_.find(k);
    bool ok = false;
    if (it != nodeClassCache_.end()) {
        ok = it->second;
    } else {
        auto& A = sr2::il2cpp::api();
        if (A.il2cpp_class_is_assignable_from) {
            ok = A.il2cpp_class_is_assignable_from(nodeClass_, k);
        } else if (A.il2cpp_class_get_parent) {
            for (Il2CppClass* c = k; c && !ok; c = A.il2cpp_class_get_parent(c)) ok = c == nodeClass_;
        } else {
            ok = k == nodeClass_;
        }
        nodeClassCache_.emplace(k, ok);
    }
    if (!ok || !unity::IsAlive(node)) return false;
    return !model || unity::Field<Il2CppObject*>(node, offNodeModel_) == model;
}

bool SpawnScaling::ApplyPace(Il2CppObject* node, double ratio, const std::string& defName) {
    float& t = unity::Field<float>(node, offNodeTime_);
    float& d = unity::Field<float>(node, offNodeDelay_);
    const bool brk = unity::Field<uint8_t>(node, offNodeBreak_) != 0;
    auto it = paces_.find(node);
    if (it != paces_.end() && (t != it->second.wroteT || d != it->second.wroteD)) {
        // Not what we wrote: another node at this address, or the game reset
        // it. What it holds now is the new vanilla value.
        paces_.erase(it);
        it = paces_.end();
    }
    if (it == paces_.end()) {
        if (!(ratio < 1.0)) return false;  // vanilla wanted and nothing of ours to undo
        if (!(t >= 0.0f && t < 3600.0f) || !(d >= 0.0f && d < 3600.0f)) return false;  // implausible / NaN
        PaceRec fresh;
        fresh.origT = fresh.wroteT = t;
        fresh.origD = fresh.wroteD = d;
        it = paces_.emplace(node, fresh).first;
    }
    PaceRec& r = it->second;
    r.sweep = sweepId_;
    r.breakMode = brk;
    r.ratio = ratio < 1.0 ? ratio : 1.0;
    auto scaled = [&](float orig) -> float {
        if (!(r.ratio < 1.0) || !(orig > 0.0f)) return orig;
        return std::max(kMinPaceSeconds, static_cast<float>(static_cast<double>(orig) * r.ratio));
    };
    // Per-item time: _timeToHarvest for vacuum nodes; for break nodes the one
    // break wait stays vanilla and the per-item coroutine delay is scaled.
    const float nt = brk ? r.origT : scaled(r.origT);
    const float nd = brk ? scaled(r.origD) : r.origD;
    if (t != nt) t = nt;
    if (d != nd) d = nd;
    r.wroteT = nt;
    r.wroteD = nd;
    if (!(r.ratio < 1.0)) {  // restored: forget it
        paces_.erase(it);
        return false;
    }
    if (!defName.empty() && pacedDefsLogged_.insert(defName).second)
        SR2_INFO("[{}] harvest pacing: {} ({}): {} {:.3f} s -> {:.3f} s per item (x{:.3f} = vanilla amount / scaled "
                 "amount; {}), so the node takes as long as its vanilla amount did",
                 kModule, defName, brk ? "break node: items pop after one wait" : "vacuum node: one item per interval",
                 brk ? "_resourceSpawnDelaySeconds" : "_timeToHarvest", brk ? r.origD : r.origT, brk ? nd : nt,
                 r.ratio,
                 brk ? std::format("break wait _timeToHarvest {:.3f} s unchanged", r.origT)
                     : std::format("_resourceSpawnDelaySeconds {:.3f} s unused", r.origD));
    return true;
}

// Before SpawnSingleResource. A new harvest session starts on the first pull
// (or when the list is not what the last pull left): bring the list to the
// current settings (still untouched: the game only reads list[0] / RemoveAt(0),
// no enumerator) and pace the node.
void SpawnScaling::BeforePull(Il2CppObject* node, PullStep& st) {
    if (!enabled_.load() || !applied_ || !cachedValid_ || !nodeListsOk_ || !paceOk_) return;
    auto* model = unity::Field<Il2CppObject*>(node, offNodeModel_);
    if (!model || unity::ClassOf(model) != modelClass_ || !IsNodeObject(node, model)) return;
    void* list = unity::Field<void*>(model, offModelList_);
    std::vector<Il2CppObject*> cur;
    if (!list || !ReadRolledList(list, cur) || cur.empty()) return;
    const int len0 = static_cast<int>(cur.size());
    auto sit = sessions_.find(node);
    if (sit == sessions_.end() || sit->second.list != list || sit->second.expectLen != len0) {
        if (sessions_.size() >= kMaxHarvestSessions) sessions_.clear();
        auto* def = unity::Field<Il2CppObject*>(model, offModelDef_);
        const float tBefore = unity::Field<float>(node, offNodeTime_);
        NodeStats ns;
        NodeRec* rec = nullptr;
        resMultCache_.clear();
        resCacheValid_ = true;
        const bool rescaled = ProcessNodeList(list, def, true, cached_, ns, &rec);
        resCacheValid_ = false;
        const double ratio = (cached_.keepHarvestTime && rec) ? rec->paceRatio : 1.0;
        std::string name;
        int32_t vMin = 0, vMax = 0, pMin = 0, pMax = 0;
        DefRanges(def, vMin, vMax, pMin, pMax, name);
        ApplyPace(node, ratio, rec ? rec->defName : name);
        std::vector<Il2CppObject*> now;
        ReadRolledList(list, now);
        HarvestSession h;
        h.list = list;
        h.startLen = h.expectLen = static_cast<int>(now.size());
        h.types = TypeSummary(now);
        h.rescaled = rescaled;
        h.origin = rec ? rec->origin : NodeOrigin::Unknown;
        h.partialBefore = rec && rec->partial;
        h.defName = std::format("{} (vanilla {}..{})", rec ? rec->defName : name, vMin, vMax);
        auto pit = paces_.find(node);
        h.vanillaT = pit != paces_.end() ? pit->second.origT : unity::Field<float>(node, offNodeTime_);
        h.pacedT = unity::Field<float>(node, offNodeTime_);
        h.firstWaitT = tBefore;
        h.ratio = pit != paces_.end() ? pit->second.ratio : 1.0;
        sit = sessions_.insert_or_assign(node, std::move(h)).first;
    }
    st.tracked = true;
    st.due = unity::Field<double>(node, offNodeHarvestAt_);
    auto pit = paces_.find(node);
    st.pace = pit != paces_.end() && !pit->second.breakMode && pit->second.ratio < 1.0;
}

// After SpawnSingleResource (the original wrote _harvestAtTime = now + t, so
// `now` is derived from it without calling Time). Returns true when a paced
// node already owes the next item in this frame.
bool SpawnScaling::AfterPull(Il2CppObject* node, PullStep& st) {
    auto sit = sessions_.find(node);
    if (sit == sessions_.end()) return false;
    HarvestSession& h = sit->second;
    auto* model = unity::Field<Il2CppObject*>(node, offNodeModel_);
    void* list = model && unity::ClassOf(model) == modelClass_ ? unity::Field<void*>(model, offModelList_) : nullptr;
    if (!list || list != h.list || unity::ClassOf(static_cast<Il2CppObject*>(list)) != rolledListClass_) {
        sessions_.erase(sit);
        return false;
    }
    const float t = unity::Field<float>(node, offNodeTime_);
    double& hat = unity::Field<double>(node, offNodeHarvestAt_);
    const double now = hat - static_cast<double>(t);
    if (h.count == 0)
        h.startTime = st.due - static_cast<double>(h.firstWaitT);
    else if (now - h.lastTime > 2.0 * static_cast<double>(t) + 0.3)
        ++h.pauses;  // the vac was off the node for a while (or a long hitch)
    ++h.count;
    h.lastTime = now;
    const int32_t len = unity::Field<int32_t>(list, offListSize_);
    h.expectLen = len;
    const int32_t state = unity::Field<int32_t>(model, offModelState_);
    if (len <= 0 || state != nodeStateHarvesting_) {
        if (len <= 0 && harvestLogs_ < kNodeLogsPerZone) {
            ++harvestLogs_;
            const double elapsed = h.startTime >= 0 ? now - h.startTime : 0.0;
            const double vanillaItems = static_cast<double>(h.startLen) * h.ratio;
            SR2_INFO("[{}] node harvested: {} gave {} item(s) ({}) in {:.2f} s{} | per item {:.3f} s (vanilla {:.3f} "
                     "s, x{:.3f}) -> vanilla amount ~{:.1f} would take ~{:.2f} s | list had {} at first pull{} [{}{}]",
                     kModule, h.defName, h.count, h.types, elapsed,
                     h.pauses ? std::format(" incl. {} pause(s) with the vac off", h.pauses) : std::string(),
                     h.pacedT, h.vanillaT, h.ratio, vanillaItems, vanillaItems * static_cast<double>(h.vanillaT),
                     h.startLen, h.rescaled ? ", rescaled just now" : "", OriginName(h.origin),
                     h.partialBefore ? ", partly harvested before" : "");
            if (harvestLogs_ == kNodeLogsPerZone)
                SR2_INFO("[{}] (further 'node harvested' lines suppressed until the next zone change)", kModule);
        }
        sessions_.erase(sit);
        return false;
    }
    if (!st.pace) return false;
    // Exact schedule: the next item is due one per-item time after THIS one
    // was due, not after the frame that noticed it (that loses up to a frame
    // per item, a lot with short paced intervals). Debt is capped.
    double next = st.due + static_cast<double>(t);
    if (next < now - kMaxCatchUpSeconds) next = now - kMaxCatchUpSeconds;
    if (next < hat) hat = next;
    st.due = hat;
    return hat <= now;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSpawnScaling() { return std::make_unique<SpawnScaling>(); }

}  // namespace sr2::modules
