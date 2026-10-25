// slime_dj: "Lamps pulse to the music" (see lamps.h, NOTES.md).
//
// Electric Pillar Lamp (round 12): GadgetDefinition "ElectricLampPillar", prefab "decorElectricLampPillar"
// (identifiabletypes bundle, UnityPy, game 1.3.0): root Gadget + RegionMember +
// SanctuaryRoomMemberGadget; "labyrinthPillar01" (glass pillar + bronze bases, LODGroup); and
// "PillarLightningFX": the lamp's light is fake, it has NO Light component:
//   * "Sphere Light" / "Sphere Light (2)" (big scaled spheres, material SphereLightPillarLightning_02,
//     a glow-volume shader: _Color (1, 0.83, 0.38), _BrightnessMultiplier 5);
//   * LightningMesh / _Rotated / Aura / Aura_Rotated / _Bottom / _Top (materials LightningPillar_*,
//     an unlit lightning shader: HDR _ColorCore (8.2, 4.5, 2.7) and _ColorEdge (4.2, 3.4, 0.75), up to
//     135 on the end caps, _Color yellow);
//   * two ParticleSystems (ChargedSpikes, untouched) and a looping SECTR_PointSource hum ("Sound").
// No script animates any of it, so property blocks on those renderers are the whole look.
//
// The other lamps (round 13; prefabs read with UnityPy, details in re/fabricator.md) have real HDRP
// Lights: Pink Striped Lamp (point Light, bulb "RoundLights" _BrightnessMultiplier + _EmissiveColor,
// stripes "Carousel" HDR _EmissiveColor), Conservatory Lamp (spot Light + cone), Blue Lava Lamp (point
// Light, glow quad _IntensityDay / _IntensityNight), Ice Lamp (ice cube with HDR _InnerEmission and a
// point Light run by a LightControl). The glowing ice crystals of Powderfall Bluffs are the same ice
// cube + LightControl Light as scene geometry ("iceCube0N_light").
// Music Pillar / Music Tile (round 16; re/world.md MusicTile): a light-up ice mesh (_InnerEmission,
// _LightControlColor), a glow mesh shown only during a hit, a point Light, and the game's MusicTile script
// that flashes the tile when something lands on it. Pulsed like the lamps, plus a beat restarts the
// script's own hit animation (MusicTile._effectAnimationTime moved back; its Update draws the flash).
// Lights are pulsed through Light.intensity / color (HDRP forwards to these). A LightControl writes the
// light in its Update (every frame when not animated, else on its ticks), in the same Update phase as
// the mod's tick (EventSystem.Update), so it overwrote the pulse (round 14 test: the ice did not pulse).
// While a lamp pulses, its LightControls are disabled and their Update is called from the pulse right
// before the light is read: the gradient, the material colour and the base intensity stay the game's,
// and the pulse is written last. Restore enables them again. The ice cube's visible glow is
// _LightControlColor on its material instance (LightControl.Awake: materialRenderer.material), read
// back each frame and scaled in the renderer's property block.
//
// Finding the lamps: GameModel.identifiablesByIdent[definition] = the GadgetModels of every placed
// lamp (loaded or not); GadgetModel.transform is the placed object (null / destroyed while its scene
// is not loaded). One dictionary lookup per definition, at most every kFindSeconds, never a scene
// search. Scene ice: one FindObjectsOfTypeAll(LightControl) per scene group (IceCheck). Lamps within
// the range of a playing speaker get a record: GC handles to their glowing renderers and lights and
// the material values. Everything else is never touched.
//
// Per frame, per channel (global queue, each Corral Radio): the analysis at its play position
// (BeatTrack::Query, as the walls read it) smoothed the walls' way (bass level attack 15 ms /
// release 100 ms, beat pulse decaying with 0.15 s, energy over 0.4 s scaling both down in calm
// parts, a hue step per beat). Per lamp, with w = strength from the speakers of its channel in range
// (kStack), e = w x level, p = w x pulse:
//   _BrightnessMultiplier / _IntensityDay / _IntensityNight = base x (1 + kGlowLevel e + kGlowBeat p)
//   _ColorCore / _ColorEdge / _EmissiveColor / _InnerEmission = base x (1 + kBoltLevel e + kBoltBeat p)
//   Light.intensity = base x (1 + kLightLevel e + kLightBeat p)
//   Electric Pillar Lamp only: every colour (also _Color, Light.color) mixed towards the hue by kColourMix x
//   max(e, p), brightness kept. The other lamps and the ice keep their own colours (round 14: "pulse lamps
//   (electric pillar can stay rgb) in their own color light").
// Restore: the renderer's own property block as it was before the first change; a light's own values.

#include "modules/slime_dj/lamps.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <unordered_map>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/common/debug_marks.h"

namespace sr2::modules::slime_dj {

namespace {
namespace unity = sr2::core::unity;
using il2cpp::api;
using il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
// Lamp decorations (gadget prefab names; their variants come along through GadgetDefinition.VariantOf).
constexpr const char* kLampPrefabs[] = {
    "decorElectricLampPillar",  // Electric Pillar Lamp: glow spheres + lightning, no Light
    "decorPinkStripedLamp",     // Pink Striped Lamp (+ Blue / Mint / Prismatic): point Light, emissive bulb and stripes
    "decorConservatoryLamp",    // Conservatory Lamp: spot Light (+ cone mesh)
    "decorSlimeLampBlue",       // Blue Lava Lamp (+ the other colours): point Light, glow quad
    "decorIceLamp",             // Ice Lamp: glowing ice cube (inner emission) + point Light run by a LightControl
    "decorMusicPillar",         // Music Pillar (round 16): light-up ice pillar + point Light, MusicTile flash on a hit
    "decorMusicTile",           // Music Tile (round 16): light-up floor tile, same parts as the pillar
};
constexpr float kFindSeconds = 4.0f;          // lamp list refresh (identifiablesByIdent), at most this often
constexpr float kNewSourceSeconds = 0.5f;     // a speaker started playing: refresh now, but at most this often
constexpr float kIceScanDelay = 5.0f;         // glowing scene ice: one scan this long after the scene group changed
constexpr size_t kMaxIce = 512;               // glowing scene ice crystals cached per scene
constexpr size_t kMaxLights = 4;              // Light components per lamp
constexpr float kDefRetrySeconds = 10.0f;     // definition not found yet (groups not loaded)
constexpr size_t kMaxLamps = 96;              // pulsing at once (nearest to a speaker first; a tile dance floor is many)
constexpr size_t kMaxModels = 2048;           // placed lamps read per refresh
constexpr size_t kMaxRends = 16;              // per lamp (the prefab has 8 glowing renderers)
constexpr float kFullShare = 0.4f;            // full closeness up to this share of the range, then fading to 0
// Strength = 1 - exp(-kStack x sum of closeness x loudness of the channel's speakers in range):
// one close speaker 0.36, two 0.59, three 0.74, four 0.83 (round 13: "lamps shouldnt be as bright
// when only one speaker close and playing").
constexpr float kStack = 0.45f;
// Look (same feel as the walls, walls.cpp)
constexpr float kAttack = 0.015f, kRelease = 0.10f;  // bass level smoothing (s)
constexpr float kPulseDecay = 0.15f;                 // beat pulse time constant (s)
constexpr float kEnergyTime = 0.4f;                  // energy smoothing (s)
constexpr float kCalmShare = 0.15f;                  // level / pulse share left in the calmest part
constexpr float kHueStep = 0.11f;                    // colour wheel step per beat
constexpr float kGlowLevel = 0.8f, kGlowBeat = 1.6f; // _BrightnessMultiplier x (1 + level a + pulse b)
constexpr float kBoltLevel = 1.0f, kBoltBeat = 2.5f; // HDR colours (_ColorCore / _ColorEdge / emission), same form
constexpr float kLightLevel = 1.0f, kLightBeat = 2.0f;  // Light.intensity, same form
constexpr float kMinEmission = 0.01f;                // an emission colour dimmer than this does not glow: left alone
constexpr float kColourMix = 0.7f;                   // hue share at full level / pulse
constexpr float kRest = 0.004f;                      // below this a channel counts as settled
constexpr float kFadeIn = 0.25f;                     // a lamp's strength eases to its distance weight with this time constant (s)
// Music Pillar / Tile flash (MusicTile._effectAnimationTime, re/world.md): a beat restarts the tile's own
// hit animation (curve: 0.5 at t 0, peak 3.37 at t 0.076, 0 at t 0.49; time units of _effectDuration).
// A weaker beat starts it further down the decay, so its flash is dimmer.
constexpr float kFlashPeak = 0.076f, kFlashEnd = 0.49f;
constexpr float kFlashMin = 0.12f;                   // beat strength (x lamp weight) below this does not flash
constexpr float kFlashGain = 1.4f;                   // beat strength x this = flash share (1 = the peak)
// Lamp placed / picked up (round 16: "on initial place it still takes time"): the lamp definitions' model
// lists (List._size / _version) and identifiablesByIdent._count are watched (Placed(), memory reads only);
// a change refreshes now and once more this much later (the GadgetModel's transform may come a frame late).
constexpr ULONGLONG kPlacedRecheckMs = 400;
constexpr const char* kRainbowPrefab = "decorElectricLampPillar";  // the one lamp that shifts hue with the beat
// Debug highlighter (modules/common/debug_marks.h)
constexpr float kDebugPublishSeconds = 1.0f;         // boxes republished this often while it is on
constexpr size_t kDebugMaxLamps = 96;                // idle lamps measured per refresh
constexpr size_t kDebugBoundsPerRefresh = 64;        // ice crystals measured per refresh (cached after)
constexpr uint32_t kColPulsing = 0xF0FF50FF, kColIdle = 0xC8E0E0E0, kColIce = 0xF0FFE060;  // IM_COL32 order

struct Vec3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};
struct Bounds {
    Vec3 center, extents;
};

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

Color Hsv(float h, float s, float v) {
    h -= std::floor(h);
    const float i = std::floor(h * 6.0f), f = h * 6.0f - i;
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p, 1};
        case 1: return {q, v, p, 1};
        case 2: return {p, v, t, 1};
        case 3: return {p, q, v, 1};
        case 4: return {t, p, v, 1};
        default: return {v, p, q, 1};
    }
}

// `base` mixed towards `hue` by k, keeping base's brightness (max channel) so a pale lamp does not
// turn darker, and its alpha.
Color Tint(const Color& base, const Color& hue, float k) {
    const float bright = std::max({base.r, base.g, base.b, 1e-4f});
    return {base.r + (hue.r * bright - base.r) * k, base.g + (hue.g * bright - base.g) * k,
            base.b + (hue.b * bright - base.b) * k, base.a};
}
}  // namespace

struct LampPulser::Impl {
    // ---- resolved
    bool ready = false;
    Il2CppClass* rendererClass = nullptr;
    Il2CppClass* groupClass = nullptr;
    Il2CppClass* gadgetDefClass = nullptr;
    Il2CppClass* gadgetModelClass = nullptr;
    Il2CppClass* mpbClass = nullptr;
    size_t offGcLookup = 0, offScGameModel = 0, offGadgetDefs = 0, offMembers = 0, offSubGroups = 0, offPrefab = 0,
           offVariantOf = 0, offByIdent = 0, offGmTransform = 0;
    const MethodInfo* gameCtxInst = nullptr;   // SRSingleton<GameContext>.get_Instance
    const MethodInfo* sceneCtxInst = nullptr;  // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* mpbCtor = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    M<void (*)(Il2CppObject*, const MethodInfo*)> mpbClear;
    M<void (*)(Il2CppObject*, int32_t, float, const MethodInfo*)> mpbSetFloat;
    M<void (*)(Il2CppObject*, int32_t, Color, const MethodInfo*)> mpbSetColor;
    M<void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> rendGetBlock, rendSetBlock;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> rendSharedMaterial;
    M<bool (*)(Il2CppObject*, int32_t, const MethodInfo*)> matHasProperty;
    M<Color (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetColor;
    M<float (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetFloat;
    M<int32_t (*)(Il2CppString*, const MethodInfo*)> propertyToId;
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetPosition;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> trGetParent;
    M<Bounds (*)(Il2CppObject*, const MethodInfo*)> rendGetBounds;  // optional: debug highlighter boxes
    // Light components (pulsed through Light.intensity / color: HDRP forwards to these, re/unity.md)
    Il2CppClass* lightClass = nullptr;
    M<float (*)(Il2CppObject*, const MethodInfo*)> lightGetIntensity;
    M<void (*)(Il2CppObject*, float, const MethodInfo*)> lightSetIntensity;
    M<Color (*)(Il2CppObject*, const MethodInfo*)> lightGetColor;
    M<void (*)(Il2CppObject*, Color, const MethodInfo*)> lightSetColor;
    // glowing scene ice (optional: without these only placed lamps pulse)
    bool iceReady = false;
    Il2CppClass* lightControlClass = nullptr;
    Il2CppClass* gadgetClass = nullptr;
    const MethodInfo* compInParent = nullptr;  // Component.GetComponentInParent(Type, bool)
    const MethodInfo* currentGroup = nullptr;  // RegionRegistry.get_CurrentSceneGroup
    size_t offScRegions = 0;
    // LightControl taken over while its lamp pulses (Ice Lamp, scene ice; optional)
    bool lcReady = false;
    size_t offLcLight = 0;                    // LightControl.thisLight
    const MethodInfo* lcUpdate = nullptr;     // LightControl.Update (private, called from the pulse)
    M<bool (*)(Il2CppObject*, const MethodInfo*)> behGetEnabled;
    M<void (*)(Il2CppObject*, bool, const MethodInfo*)> behSetEnabled;
    // Music Pillar / Tile: their MusicTile's hit animation clock (optional: without it they only glow)
    Il2CppClass* musicTileClass = nullptr;
    size_t offTileAnimTime = 0;  // MusicTile._effectAnimationTime
    // the live Dictionary<IdentifiableType, List<IdentifiableModel>> class (generic: looked up on it)
    Il2CppClass* dictClass = nullptr;
    const MethodInfo* dictTryGet = nullptr;
    size_t offDictCount = 0;                  // Dictionary._count (0 = unknown: not watched)
    Il2CppClass* listClass = nullptr;         // List<IdentifiableModel>
    size_t offListSize = 0, offListVersion = 0;
    // _BrightnessMultiplier, _IntensityDay, _IntensityNight (floats: glow); _Color (tint only);
    // _ColorCore, _ColorEdge, _EmissiveColor, _InnerEmission (HDR colours: scaled and tinted);
    // _LightControlColor (the ice glow: LightControl animates it on the material, so it is read back
    // from the material every frame instead of kept from Build)
    enum Prop { kBright, kDay, kNight, kTint, kCore, kEdge, kEmissive, kInner, kLcColor, kProps };
    static bool IsFloat(int k) { return k == kBright || k == kDay || k == kNight; }
    static bool IsHdr(int k) { return k == kCore || k == kEdge || k == kEmissive || k == kInner || k == kLcColor; }
    int32_t ids[kProps] = {};

    // ---- state
    struct RendRec {
        Il2CppGCHandle h = nullptr, saved = nullptr;  // saved: the renderer's own block
        bool has[kProps] = {};
        float val[kProps] = {};  // floats
        Color col[kProps] = {};  // colours
    };
    struct LightRec {
        Il2CppGCHandle h = nullptr;
        Il2CppGCHandle lc = nullptr;  // the LightControl that drives it (nullptr = none)
        bool lcOff = false;           // the pulse disabled that LightControl and runs its Update itself
        float base = 0, wrote = 0;    // its own intensity / what the pulse wrote last
        Color baseCol{}, wroteCol{};
    };
    struct Lamp {
        Il2CppGCHandle tr = nullptr;  // the placed lamp's Transform (identity + alive check)
        Vec3 pos{};
        std::vector<RendRec> rends;
        std::vector<LightRec> lights;
        std::vector<Il2CppGCHandle> tiles;  // MusicTile components (Music Pillar / Tile): beat flash
        const void* channel = nullptr;  // nearest playing speaker's channel
        float weight = 0;               // 0..1 from its distance to that speaker
        float shown = 0;                // weight eased in (a lamp entering range fades in)
        bool touched = false;
        bool rainbow = false;           // Electric Pillar Lamp: hue shift with the beat (others keep their colour)
        bool ice = false;               // glowing scene ice crystal (for the log)
        Bounds box{};                   // world bounds of its renderers (debug highlighter); extents 0 = unknown
    };
    struct ChannelState {
        uint64_t last = 0;
        unsigned gen = 0;
        float env = 0, pulse = 0, energy = 1, hue = 0;
        float beatNow = 0;    // strength of a beat that arrived this frame (0 = none)
        bool active = false;  // has a beat sample this frame
        bool seen = false;    // listed this frame
    };
    std::vector<Lamp> lamps;
    std::unordered_map<const void*, ChannelState> channels;
    std::vector<Il2CppGCHandle> defs;  // lamp definitions (base + variants)
    ULONGLONG defSearch = 0, lastFind = 0;
    bool defsLogged = false, defsMissingLogged = false;
    Il2CppGCHandle work = nullptr;     // shared MaterialPropertyBlock
    std::vector<std::pair<Il2CppObject*, Vec3>> placed;  // Find: lamp transforms (raw, valid during this Sync) + position
    size_t placedLamps = 0;            // Find: the first placedLamps of `placed` are lamps, the rest scene ice
    size_t loaded = 0;                 // lamps loaded at the last refresh (for the log)
    size_t lastLogged = 0;
    int builtLogs = 0, iceLogs = 0, tileLogs = 0;
    bool lcLogged = false;
    std::vector<std::pair<const void*, Vec3>> lastSources;  // the previous Sync's sources (a new one refreshes now)
    // placement watch (Placed): the dictionary and each definition's model list as of the last Find
    struct Watch {
        Il2CppGCHandle list = nullptr;  // nullptr = the definition had no list then
        int size = 0, version = 0;
    };
    std::vector<Watch> watch;
    Il2CppGCHandle watchDict = nullptr;
    int watchCount = 0;
    bool forceFind = false;     // Placed saw a change: the next Sync refreshes the lamp list now
    ULONGLONG recheckAt = 0;    // a second refresh after a change (0 = none)
    int placedLogs = 0;
    // glowing scene ice of the current scene group: the crystal's Transform + position (static geometry)
    std::vector<std::pair<Il2CppGCHandle, Vec3>> ice;
    std::vector<Bounds> iceBox;      // same index as `ice`: renderer bounds (debug highlighter; extents 0 = not measured)
    const void* iceGroup = nullptr;  // scene group the cache belongs to (compared only, never dereferenced)
    ULONGLONG iceScanAt = 0;         // pending scan time (0 = none)
    std::string iceGroupName;        // for the log
    // debug highlighter
    bool debugOn = false;
    ULONGLONG debugFind = 0, debugPublish = 0;
    struct DebugLamp {
        Vec3 pos;
        Bounds box;
        Il2CppGCHandle tr;  // identity: skipped while it pulses (drawn as pulsing)
    };
    std::vector<DebugLamp> debugIdle;
    std::vector<bool> iceOn;         // same index as `ice`: loaded (active) at the last debug refresh
    std::string debugLogged;         // last highlighter summary logged

    template <class Fn>
    static bool Bind(M<Fn>& m, const MethodInfo* mi) {
        void* p = mi ? Resolver::Get().MethodPointer(mi) : nullptr;
        if (!p) return false;
        m.fn = reinterpret_cast<Fn>(p);
        m.mi = mi;
        return true;
    }
    static Il2CppObject* Target(Il2CppGCHandle h) {
        Il2CppObject* o = h ? unity::GCHandleTarget(h) : nullptr;
        return unity::IsAlive(o) ? o : nullptr;
    }
    static void Free(Il2CppGCHandle& h) {
        if (h) unity::FreeGCHandle(h);
        h = nullptr;
    }
    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok) {
        if (!m) {
            *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        *ok = err.empty();
        if (!*ok) {
            if (err.rfind("native exception", 0) == 0) core::MarkModuleFaulted(kModule, std::format("lamps: {}: {}", what, err));
            else SR2_WARN("slime_dj: lamps: {} failed: {}", what, err);
        }
        return r;
    }
    Il2CppObject* NewBlock() {
        Il2CppObject* b = objectNew(mpbClass);
        bool ok = false;
        if (b) Invoke(mpbCtor, b, {}, "MaterialPropertyBlock..ctor", &ok);
        return ok ? b : nullptr;
    }
    Vec3 Position(Il2CppObject* tr, bool* ok) {
        const Vec3 p = trGetPosition.fn(tr, trGetPosition.mi);
        *ok = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        return p;
    }

    // ---- definitions: the lamp's GadgetDefinition and its variants, from the gadget groups.
    bool Defs() {
        for (size_t i = 0; i < defs.size();)
            if (Target(defs[i])) {
                ++i;
            } else {
                Free(defs[i]);
                defs.erase(defs.begin() + static_cast<std::ptrdiff_t>(i));
            }
        if (!defs.empty()) return true;
        const ULONGLONG now = GetTickCount64();
        if (defSearch && now - defSearch < static_cast<ULONGLONG>(kDefRetrySeconds * 1000)) return false;
        defSearch = now;
        bool ok = false;
        Il2CppObject* gc = Invoke(gameCtxInst, nullptr, {}, "GameContext.Instance", &ok);
        Il2CppObject* lookup = ok && unity::IsAlive(gc) ? unity::Field<Il2CppObject*>(gc, offGcLookup) : nullptr;
        if (!unity::IsAlive(lookup)) return false;
        std::vector<Il2CppObject*> groups{unity::Field<Il2CppObject*>(lookup, offGadgetDefs)}, all, bases;
        for (size_t gi = 0; gi < groups.size() && gi < 64; ++gi) {
            Il2CppObject* grp = groups[gi];
            if (!unity::IsAlive(grp) || !IsA(grp, groupClass)) continue;
            void* members = unity::Field<void*>(grp, offMembers);
            for (size_t i = 0, n = unity::ListCount(members); i < n && i < 2000; ++i) {
                auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
                if (!unity::IsAlive(t) || !IsA(t, gadgetDefClass)) continue;
                all.push_back(t);
                Il2CppObject* prefab = unity::Field<Il2CppObject*>(t, offPrefab);
                if (!unity::IsAlive(prefab) || std::find(bases.begin(), bases.end(), t) != bases.end()) continue;
                const std::string pn = unity::GetName(prefab);
                if (std::any_of(std::begin(kLampPrefabs), std::end(kLampPrefabs), [&](const char* k) { return pn == k; }))
                    bases.push_back(t);
            }
            void* subs = unity::Field<void*>(grp, offSubGroups);
            for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
                groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
        }
        if (bases.empty()) {
            if (!all.empty() && !defsMissingLogged) {
                defsMissingLogged = true;
                SR2_WARN("slime_dj: lamps: no gadget definition with a lamp prefab ('{}', ...) among {} (retried every {:.0f} s)",
                         kLampPrefabs[0], all.size(), kDefRetrySeconds);
            }
            return false;
        }
        std::string names;
        for (Il2CppObject* t : all) {
            Il2CppObject* of = unity::Field<Il2CppObject*>(t, offVariantOf);
            if (std::find(bases.begin(), bases.end(), t) == bases.end() && (!of || std::find(bases.begin(), bases.end(), of) == bases.end()))
                continue;
            if (std::any_of(defs.begin(), defs.end(), [&](Il2CppGCHandle h) { return Target(h) == t; })) continue;
            if (Il2CppGCHandle h = unity::NewGCHandle(t)) {
                defs.push_back(h);
                names += (names.empty() ? "" : ", ") + unity::GetName(t);
            }
        }
        if (!defsLogged && !defs.empty()) {
            defsLogged = true;
            SR2_INFO("slime_dj: lamps: {} lamp definition(s) ({} of {} lamp kinds found): {}", defs.size(), bases.size(),
                     std::size(kLampPrefabs), names);
        }
        return !defs.empty();
    }

    // ---- every placed lamp that is loaded now (transform + position).
    void Find() {
        placed.clear();
        placedLamps = 0;
        if (!Defs()) return;
        bool ok = false;
        Il2CppObject* sc = Invoke(sceneCtxInst, nullptr, {}, "SceneContext.Instance", &ok);
        Il2CppObject* gm = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel) : nullptr;
        Il2CppObject* dict = unity::IsAlive(gm) ? unity::Field<Il2CppObject*>(gm, offByIdent) : nullptr;
        if (!dict) return;
        if (Il2CppClass* k = unity::ClassOf(dict); k != dictClass) {
            dictClass = k;
            dictTryGet = k ? Resolver::Get().Method(k, "TryGetValue", 2, nullptr) : nullptr;
            const auto count = k ? Resolver::Get().FieldOffset(k, "_count", nullptr) : std::nullopt;
            offDictCount = count ? *count : 0;
        }
        if (!dictTryGet) return;
        std::vector<Il2CppObject*> lists;  // per definition (placement watch)
        for (Il2CppGCHandle h : defs) {
            Il2CppObject* def = Target(h);
            if (!def) continue;
            Il2CppObject* list = nullptr;
            Il2CppObject* boxed = Invoke(dictTryGet, dict, {def, &list}, "identifiablesByIdent.TryGetValue", &ok);
            if (!ok) return;
            const bool found = boxed && *reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
            lists.push_back(found ? list : nullptr);  // a managed List (no m_CachedPtr, so no IsAlive)
            for (size_t i = 0, n = found && list ? unity::ListCount(list) : 0; i < n && placed.size() < kMaxModels; ++i) {
                auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
                if (!m || !IsA(m, gadgetModelClass)) continue;
                Il2CppObject* tr = unity::Field<Il2CppObject*>(m, offGmTransform);
                if (!unity::IsAlive(tr) || !unity::ActiveInHierarchy(unity::GameObjectOf(tr))) continue;
                bool posOk = false;
                const Vec3 p = Position(tr, &posOk);
                if (posOk) placed.emplace_back(tr, p);
            }
        }
        placedLamps = placed.size();
        Rewatch(dict, lists);
        // glowing scene ice that is loaded now (positions from the scan: scene geometry does not move)
        for (const auto& [h, p] : ice) {
            if (placed.size() >= kMaxModels) break;
            Il2CppObject* tr = Target(h);
            if (tr && unity::ActiveInHierarchy(unity::GameObjectOf(tr))) placed.emplace_back(tr, p);
        }
    }

    // ---- placement watch: remembers the dictionary's key count and each lamp definition's model list
    // (size + version) as Find saw them; Changed compares them with plain memory reads.
    void FreeWatch() {
        for (Watch& w : watch) Free(w.list);
        watch.clear();
        Free(watchDict);
        watchCount = 0;
    }
    void Rewatch(Il2CppObject* dict, const std::vector<Il2CppObject*>& lists) {
        if (Changed()) forceFind = true;  // a Find of the debug highlighter must not swallow a placement
        FreeWatch();
        if (!offDictCount) return;
        for (Il2CppObject* l : lists) {
            if (l && unity::ClassOf(l) != listClass) {
                listClass = unity::ClassOf(l);
                const auto size = Resolver::Get().FieldOffset(listClass, "_size", nullptr);
                const auto version = Resolver::Get().FieldOffset(listClass, "_version", nullptr);
                offListSize = size ? *size : 0;
                offListVersion = version ? *version : 0;
            }
            if (l && (!offListSize || !offListVersion)) return;  // not watchable: the timers alone refresh
        }
        watchDict = unity::NewGCHandle(dict);
        if (!watchDict) return;
        watchCount = unity::Field<int>(dict, offDictCount);
        for (Il2CppObject* l : lists) {
            Watch w;
            w.list = l ? unity::NewGCHandle(l) : nullptr;
            if (w.list) {
                w.size = unity::Field<int>(l, offListSize);
                w.version = unity::Field<int>(l, offListVersion);
            }
            watch.push_back(w);
        }
    }
    // managed collections (not UnityEngine.Object): the handle's target as is
    static Il2CppObject* Managed(Il2CppGCHandle h) { return h ? unity::GCHandleTarget(h) : nullptr; }
    bool Changed() {
        Il2CppObject* dict = Managed(watchDict);
        if (!dict) return false;
        if (unity::Field<int>(dict, offDictCount) != watchCount) return true;
        for (const Watch& w : watch)
            if (Il2CppObject* l = Managed(w.list))
                if (unity::Field<int>(l, offListSize) != w.size || unity::Field<int>(l, offListVersion) != w.version) return true;
        return false;
    }

    // ---- glowing scene ice (Powderfall Bluffs "iceCube0N_light"): a LightControl-driven point Light
    // under an ice cube with an inner-emission material. One FindObjectsOfTypeAll(LightControl) per
    // scene group (kIceScanDelay after it changed), only while a speaker plays or the debug highlighter
    // is on; never per frame.
    // Round 13 test ("didnt see"): both scans in the log ran on the ranch (ConservatoryFields, 0 crystals of
    // 20 LightControls, the placed Ice Lamp skipped as a gadget); in Powderfall no speaker played (0 speakers
    // in the minute line), so no lamp refresh ran there and the zone was never scanned. The highlighter now
    // scans without music, and the log names the scene group (and, with 0 found, some LightControl parents).
    void IceCheck() {
        if (!iceReady) return;
        bool ok = false;
        Il2CppObject* sc = Invoke(sceneCtxInst, nullptr, {}, "SceneContext.Instance", &ok);
        Il2CppObject* regions = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScRegions) : nullptr;
        Il2CppObject* group = unity::IsAlive(regions) ? Invoke(currentGroup, regions, {}, "RegionRegistry.CurrentSceneGroup", &ok) : nullptr;
        if (!ok || !group) return;
        const ULONGLONG now = GetTickCount64();
        if (group != iceGroup) {
            iceGroup = group;
            iceGroupName = unity::GetName(group);
            FreeIce();
            iceScanAt = now + static_cast<ULONGLONG>(kIceScanDelay * 1000);
            return;
        }
        if (!iceScanAt || now < iceScanAt) return;
        iceScanAt = 0;
        const LARGE_INTEGER t0 = Now();
        size_t seen = 0, gadgets = 0;
        std::vector<std::string> others;  // parents of the LightControls that did not match (log, with 0 found)
        for (Il2CppObject* lc : unity::FindObjectsOfTypeAll(lightControlClass)) {
            if (ice.size() >= kMaxIce || ++seen > 8192) break;
            if (!unity::IsAlive(lc)) continue;
            Il2CppObject* tr = unity::TransformOf(lc);
            Il2CppObject* cube = tr ? trGetParent.fn(tr, trGetParent.mi) : nullptr;
            if (!unity::IsAlive(cube)) continue;
            const std::string name = unity::GetName(cube);
            if (_strnicmp(name.c_str(), "iceCube", 7) != 0) {
                if (others.size() < 8 && std::find(others.begin(), others.end(), name) == others.end()) others.push_back(name);
                continue;
            }
            bool inactive = true;
            Il2CppObject* gadget = Invoke(compInParent, lc, {unity::TypeOf(gadgetClass), &inactive}, "GetComponentInParent", &ok);
            if (!ok) return;
            if (unity::IsAlive(gadget)) {  // an Ice Lamp decoration: pulsed as a placed lamp
                ++gadgets;
                continue;
            }
            bool posOk = false;
            const Vec3 p = Position(cube, &posOk);
            if (!posOk) continue;
            if (Il2CppGCHandle h = unity::NewGCHandle(cube)) {
                ice.emplace_back(h, p);
                iceBox.push_back({});
            }
        }
        std::string other;
        if (ice.empty())
            for (const std::string& n : others) other += std::format("{}'{}'", other.empty() ? "; other LightControl parents: " : ", ", n);
        SR2_INFO("slime_dj: lamps: {} glowing ice crystal(s) in scene group '{}' ({} LightControl(s) looked at, {} on placed Ice "
                 "Lamps, {:.2f} ms{})",
                 ice.size(), iceGroupName, seen, gadgets, Ms(t0), other);
    }
    void FreeIce() {
        for (auto& [h, p] : ice) Free(h);
        ice.clear();
        iceBox.clear();
        iceOn.clear();
    }
    static LARGE_INTEGER Now() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t;
    }
    static double Ms(LARGE_INTEGER t0) {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        return 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    }

    std::vector<Il2CppObject*> Children(Il2CppObject* tr, Il2CppClass* k, size_t max) {
        std::vector<Il2CppObject*> out;
        bool inactive = false, ok = false;
        Il2CppObject* arr = Invoke(compsInChildren, tr, {unity::TypeOf(k), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? std::min<size_t>(unity::ArrayLength(arr), 64) : 0; i < n && out.size() < max; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (unity::IsAlive(c) && IsA(c, k)) out.push_back(c);
        }
        return out;
    }

    // World bounds of the given renderers (debug highlighter boxes); extents 0 when none / unavailable.
    Bounds Measure(const std::vector<Il2CppObject*>& rends) {
        Bounds out{};
        if (!rendGetBounds) return out;
        Vec3 lo{}, hi{};
        bool any = false;
        for (Il2CppObject* r : rends) {
            const Bounds b = rendGetBounds.fn(r, rendGetBounds.mi);
            const Vec3 c = b.center, e = b.extents;
            const float sum = e.x + e.y + e.z;
            if (!std::isfinite(c.x + c.y + c.z + sum) || sum <= 0 || sum > 300) continue;
            const Vec3 a{c.x - e.x, c.y - e.y, c.z - e.z}, z{c.x + e.x, c.y + e.y, c.z + e.z};
            lo = any ? Vec3{std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)} : a;
            hi = any ? Vec3{std::max(hi.x, z.x), std::max(hi.y, z.y), std::max(hi.z, z.z)} : z;
            any = true;
        }
        if (any)
            out = {{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f},
                   {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f}};
        return out;
    }

    bool Build(Lamp& lamp, Il2CppObject* tr) {
        std::string glow;
        const std::string name = unity::GetName(tr);
        lamp.rainbow = name.rfind(kRainbowPrefab, 0) == 0;
        lamp.ice = _strnicmp(name.c_str(), "iceCube", 7) == 0;
        const std::vector<Il2CppObject*> all = Children(tr, rendererClass, 48);
        lamp.box = Measure(all);
        for (Il2CppObject* r : all) {
            if (lamp.rends.size() >= kMaxRends) break;
            Il2CppObject* mat = rendSharedMaterial.fn(r, rendSharedMaterial.mi);
            if (!unity::IsAlive(mat)) continue;
            RendRec rec;
            for (int k = 0; k < kProps; ++k) {
                rec.has[k] = ids[k] && matHasProperty.fn(mat, ids[k], matHasProperty.mi);
                if (!rec.has[k]) continue;
                if (IsFloat(k)) {
                    rec.val[k] = matGetFloat.fn(mat, ids[k], matGetFloat.mi);
                    rec.has[k] = std::isfinite(rec.val[k]);
                } else {
                    const Color& c = rec.col[k] = matGetColor.fn(mat, ids[k], matGetColor.mi);
                    rec.has[k] = std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
                    // emission that is off (black: a lamp body, snow, the cone) is left alone
                    if (rec.has[k] && (k == kEmissive || k == kInner)) rec.has[k] = std::max({c.r, c.g, c.b}) > kMinEmission;
                }
            }
            // only glowing parts: glow volumes (_BrightnessMultiplier, _IntensityDay / Night), lightning
            // (_ColorCore / _ColorEdge) and emission that is on; the pillar glass, bases, lamp bodies and
            // grass quads have none of these. _Color (tint) only on glow volumes and lightning: elsewhere
            // it is the albedo.
            const bool volume = rec.has[kBright] || rec.has[kCore] || rec.has[kEdge];
            if (!volume) rec.has[kTint] = false;
            if (!volume && !rec.has[kDay] && !rec.has[kNight] && !rec.has[kEmissive] && !rec.has[kInner] && !rec.has[kLcColor]) continue;
            Il2CppObject* saved = NewBlock();
            if (!saved) continue;
            mpbClear.fn(saved, mpbClear.mi);
            rendGetBlock.fn(r, saved, rendGetBlock.mi);  // the renderer's own block, set back on restore
            rec.h = unity::NewGCHandle(r);
            rec.saved = unity::NewGCHandle(saved);
            if (!rec.h || !rec.saved) {
                Free(rec.h);
                Free(rec.saved);
                continue;
            }
            if (glow.size() < 240) {
                static constexpr const char* kShort[kProps] = {"brightness", "day", "night", "tint", "core", "edge", "emissive", "inner", "lightcontrol"};
                std::string what;
                for (int k = 0; k < kProps; ++k)
                    if (rec.has[k] && k != kTint) {
                        const Color& c = rec.col[k];
                        what = IsFloat(k) ? std::format("{} {:.2f}", kShort[k], rec.val[k])
                                          : std::format("{} ({:.1f}, {:.1f}, {:.1f})", kShort[k], c.r, c.g, c.b);
                        break;
                    }
                glow += std::format("{}'{}' {}", glow.empty() ? "" : ", ", unity::GetName(mat), what);
            }
            lamp.rends.push_back(rec);
        }
        // LightControls of this lamp, matched to their light by LightControl.thisLight
        const std::vector<Il2CppObject*> lcs = lcReady ? Children(tr, lightControlClass, 8) : std::vector<Il2CppObject*>{};
        for (Il2CppObject* l : Children(tr, lightClass, kMaxLights)) {
            LightRec rec;
            rec.h = unity::NewGCHandle(l);
            if (!rec.h) continue;
            auto lc = std::find_if(lcs.begin(), lcs.end(), [&](Il2CppObject* c) { return unity::Field<Il2CppObject*>(c, offLcLight) == l; });
            if (lc != lcs.end()) rec.lc = unity::NewGCHandle(*lc);
            if (glow.size() < 300)
                glow += std::format("{}light {:.0f}{}", glow.empty() ? "" : ", ", lightGetIntensity.fn(l, lightGetIntensity.mi),
                                    rec.lc ? " (LightControl)" : "");
            lamp.lights.push_back(rec);
        }
        // Music Pillar / Tile: the MusicTile whose hit animation a beat restarts
        if (musicTileClass && offTileAnimTime)
            for (Il2CppObject* t : Children(tr, musicTileClass, 2))
                if (Il2CppGCHandle h = unity::NewGCHandle(t)) lamp.tiles.push_back(h);
        if (lamp.rends.empty() && lamp.lights.empty() && lamp.tiles.empty()) return false;
        if (lamp.ice ? iceLogs++ < 4 : !lamp.tiles.empty() ? tileLogs++ < 4 : builtLogs++ < 8)
            SR2_INFO("slime_dj: lamps: pulsing lamp '{}' at ({:.0f}, {:.0f}, {:.0f}): {} glowing renderer(s), {} light(s){}: {}",
                     unity::GetName(tr), lamp.pos.x, lamp.pos.y, lamp.pos.z, lamp.rends.size(), lamp.lights.size(),
                     lamp.tiles.empty() ? "" : ", beat flash (MusicTile)", glow);
        return true;
    }

    void Restore(Lamp& lamp) {
        if (!lamp.touched) return;
        lamp.touched = false;
        for (const RendRec& r : lamp.rends) {
            Il2CppObject* o = Target(r.h);
            Il2CppObject* saved = Target(r.saved);
            if (o && saved) rendSetBlock.fn(o, saved, rendSetBlock.mi);
        }
        // a light is put back only while it still shows the pulse's last value (a LightControl or the
        // game may have set its own since; then that one stays)
        for (LightRec& r : lamp.lights) {
            Il2CppObject* o = Target(r.h);
            if (o) {
                if (Same(lightGetIntensity.fn(o, lightGetIntensity.mi), r.wrote)) lightSetIntensity.fn(o, r.base, lightSetIntensity.mi);
                if (Same(lightGetColor.fn(o, lightGetColor.mi), r.wroteCol)) lightSetColor.fn(o, r.baseCol, lightSetColor.mi);
            }
            ReleaseControl(r, o);
        }
    }
    // Hands a light back to its LightControl. LightControl.OnEnable also switches the Light on, so the
    // Light's own state (CullablePointLight turns far lights off) is put back after it.
    void ReleaseControl(LightRec& r, Il2CppObject* light) {
        if (!r.lcOff) return;
        r.lcOff = false;
        Il2CppObject* lc = Target(r.lc);
        if (!lc) return;
        const bool lightOn = light && behGetEnabled.fn(light, behGetEnabled.mi);
        behSetEnabled.fn(lc, true, behSetEnabled.mi);
        if (light && !lightOn) behSetEnabled.fn(light, false, behSetEnabled.mi);
    }
    // Takes a light over from its LightControl for this frame: disabled (Unity no longer runs its
    // Update in some order against the mod's tick), then its Update runs here, so the light and the
    // ice material show the game's own animation and the pulse is applied after it. A LightControl the
    // game disabled itself (a non-looping one that ended) is left alone.
    void RunControl(LightRec& r) {
        Il2CppObject* lc = Target(r.lc);
        if (!lc || !lcUpdate) return;
        if (behGetEnabled.fn(lc, behGetEnabled.mi)) {
            behSetEnabled.fn(lc, false, behSetEnabled.mi);
            if (!r.lcOff && !lcLogged) {
                lcLogged = true;
                SR2_INFO("slime_dj: lamps: took over a LightControl light (its Update now runs right before the pulse)");
            }
            r.lcOff = true;
        }
        if (!r.lcOff) return;
        bool ok = false;
        Invoke(lcUpdate, lc, {}, "LightControl.Update", &ok);
        if (!ok) ReleaseControl(r, Target(r.h));
    }
    static bool Same(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max({std::fabs(a), std::fabs(b), 1.0f}); }
    static bool Same(const Color& a, const Color& b) { return Same(a.r, b.r) && Same(a.g, b.g) && Same(a.b, b.b); }

    void Drop(Lamp& lamp) {
        Restore(lamp);
        for (RendRec& r : lamp.rends) {
            Free(r.h);
            Free(r.saved);
        }
        lamp.rends.clear();
        for (LightRec& r : lamp.lights) {
            Free(r.h);
            Free(r.lc);
        }
        lamp.lights.clear();
        for (Il2CppGCHandle& h : lamp.tiles) Free(h);
        lamp.tiles.clear();
        Free(lamp.tr);
    }

    void Animate(Lamp& lamp, const ChannelState& ch) {
        Il2CppObject* work0 = Target(work);
        if (!work0) return;
        const float w = lamp.shown;
        const float e = ch.env * w, p = ch.pulse * w;
        const Color hue = Hsv(ch.hue, 0.85f, 1.0f);
        const float k = lamp.rainbow ? kColourMix * std::min(1.0f, std::max(e, p)) : 0.0f;  // the others keep their colour
        const float glow = 1.0f + kGlowLevel * e + kGlowBeat * p, bolt = 1.0f + kBoltLevel * e + kBoltBeat * p;
        const float light = 1.0f + kLightLevel * e + kLightBeat * p;
        const bool fresh = !lamp.touched;  // first frame after a restore: read the lights' own values
        lamp.touched = true;
        // LightControls first: their Update sets the ice material colour and the light this frame
        for (LightRec& r : lamp.lights)
            if (r.lc) RunControl(r);
        for (const RendRec& r : lamp.rends) {
            Il2CppObject* o = Target(r.h);
            if (!o) continue;
            Color lcCol{};
            if (r.has[kLcColor]) {  // the LightControl's current colour on the material (animated)
                Il2CppObject* mat = rendSharedMaterial.fn(o, rendSharedMaterial.mi);
                lcCol = unity::IsAlive(mat) ? matGetColor.fn(mat, ids[kLcColor], matGetColor.mi) : r.col[kLcColor];
                if (!std::isfinite(lcCol.r) || !std::isfinite(lcCol.g) || !std::isfinite(lcCol.b)) lcCol = r.col[kLcColor];
            }
            mpbClear.fn(work0, mpbClear.mi);
            rendGetBlock.fn(o, work0, rendGetBlock.mi);  // keeps whatever else the game put there
            for (int c = 0; c < kProps; ++c) {
                if (!r.has[c]) continue;
                if (IsFloat(c)) {
                    mpbSetFloat.fn(work0, ids[c], r.val[c] * glow, mpbSetFloat.mi);
                } else {
                    const Color t = Tint(c == kLcColor ? lcCol : r.col[c], hue, k);
                    const float s = IsHdr(c) ? bolt : 1.0f;
                    mpbSetColor.fn(work0, ids[c], Color{t.r * s, t.g * s, t.b * s, t.a}, mpbSetColor.mi);
                }
            }
            rendSetBlock.fn(o, work0, rendSetBlock.mi);
        }
        // Lights: base x factor. A value the pulse did not write (a LightControl's gradient tick, the
        // game) becomes the new base, so nothing fights the game's own animation.
        for (LightRec& r : lamp.lights) {
            Il2CppObject* o = Target(r.h);
            if (!o) continue;
            const float cur = lightGetIntensity.fn(o, lightGetIntensity.mi);
            const Color col = lightGetColor.fn(o, lightGetColor.mi);
            if (!std::isfinite(cur) || !std::isfinite(col.r) || !std::isfinite(col.g) || !std::isfinite(col.b)) continue;
            if (fresh || !Same(cur, r.wrote)) r.base = cur;
            if (fresh || !Same(col, r.wroteCol)) r.baseCol = col;
            r.wrote = r.base * light;
            r.wroteCol = Tint(r.baseCol, hue, k);
            lightSetIntensity.fn(o, r.wrote, lightSetIntensity.mi);
            if (lamp.rainbow) lightSetColor.fn(o, r.wroteCol, lightSetColor.mi);  // the others' own colour is left alone
        }
        // Music Pillar / Tile: a beat restarts the tile's own hit animation (MusicTile.Update then brightens
        // the tile material and shows the glow renderer, as when something bounces on it; no sound). A
        // weaker beat (or a lamp further away) starts it further down the decay. Only ever moves the clock
        // back (brighter), so a hit by the player or a slime still plays out in full.
        if (!lamp.tiles.empty() && ch.beatNow > 0) {
            const float s = std::min(1.0f, ch.beatNow * w * kFlashGain);
            if (s >= kFlashMin) {
                const float t = kFlashPeak + (kFlashEnd - kFlashPeak) * (1.0f - s);
                for (Il2CppGCHandle h : lamp.tiles)
                    if (Il2CppObject* tile = Target(h)) {
                        float& clock = unity::Field<float>(tile, offTileAnimTime);
                        if (!std::isfinite(clock) || clock > t) clock = t;
                    }
            }
        }
    }
};

LampPulser::LampPulser() : p_(std::make_unique<Impl>()) {}
LampPulser::~LampPulser() = default;  // never runs for the module's instance (leaked on purpose)

bool LampPulser::Ready() const { return p_->ready; }
size_t LampPulser::Lamps() const { return p_->lamps.size(); }

bool LampPulser::Placed() {
    Impl& P = *p_;
    if (!P.ready) return false;
    if (P.forceFind) return true;  // still pending (a Find of the debug highlighter saw the change)
    const ULONGLONG now = GetTickCount64();
    if (P.recheckAt && now >= P.recheckAt) {  // the second refresh after a change
        P.recheckAt = 0;
        P.forceFind = true;
        return true;
    }
    if (!P.Changed()) return false;
    P.FreeWatch();  // the refresh watches again; nothing more until then
    P.forceFind = true;
    P.recheckAt = now + kPlacedRecheckMs;
    if (P.placedLogs++ < 3) SR2_INFO("slime_dj: lamps: a lamp was placed or picked up; lamp list refreshed now");
    return true;
}

bool LampPulser::Resolve(std::string& why) {
    Impl& P = *p_;
    Resolver& R = Resolver::Get();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = std::format("class {}.{}: {}", ns, name, err);
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) -> const MethodInfo* {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto bind = [&](auto& m, const MethodInfo* mi, const char* what) {
        if (!Impl::Bind(m, mi) && why.empty()) why = std::format("{}: no native body", what);
    };

    HMODULE ga = api().module;
    P.objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!P.objectNew) why = "il2cpp_object_new export missing";
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* identType = cls(kImage, "", "IdentifiableType");
    P.groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    P.gadgetDefClass = cls(kImage, "", "GadgetDefinition");
    Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel");
    P.gadgetModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GadgetModel");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* shader = cls(kCore, "UnityEngine", "Shader");
    P.rendererClass = cls(kCore, "UnityEngine", "Renderer");
    P.mpbClass = cls(kCore, "UnityEngine", "MaterialPropertyBlock");
    if (!why.empty()) return false;
    off(gameCtx, "<LookupDirector>k__BackingField", P.offGcLookup);
    off(sceneCtx, "<GameModel>k__BackingField", P.offScGameModel);
    off(lookup, "_gadgetDefinitions", P.offGadgetDefs);
    off(P.groupClass, "_memberTypes", P.offMembers);
    off(P.groupClass, "_memberGroups", P.offSubGroups);
    off(identType, "prefab", P.offPrefab);
    off(P.gadgetDefClass, "VariantOf", P.offVariantOf);
    off(gameModel, "identifiablesByIdent", P.offByIdent);
    off(P.gadgetModelClass, "transform", P.offGmTransform);
    if (!why.empty()) return false;
    P.gameCtxInst = R.Method(parentOf(gameCtx), "get_Instance", 0, nullptr);
    P.sceneCtxInst = R.Method(parentOf(sceneCtx), "get_Instance", 0, nullptr);
    if (!P.gameCtxInst || !P.sceneCtxInst) why = "SRSingleton.get_Instance not found";
    P.compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    P.mpbCtor = byParams(P.mpbClass, ".ctor", {});
    bind(P.mpbClear, byParams(P.mpbClass, "Clear", {}), "MaterialPropertyBlock.Clear");
    bind(P.mpbSetFloat, byParams(P.mpbClass, "SetFloat", {"System.Int32", "System.Single"}), "MaterialPropertyBlock.SetFloat");
    bind(P.mpbSetColor, byParams(P.mpbClass, "SetColor", {"System.Int32", "UnityEngine.Color"}), "MaterialPropertyBlock.SetColor");
    bind(P.rendGetBlock, byParams(P.rendererClass, "GetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.GetPropertyBlock");
    bind(P.rendSetBlock, byParams(P.rendererClass, "SetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.SetPropertyBlock");
    bind(P.rendSharedMaterial, byParams(P.rendererClass, "get_sharedMaterial", {}), "Renderer.get_sharedMaterial");
    bind(P.matHasProperty, byParams(material, "HasProperty", {"System.Int32"}), "Material.HasProperty");
    bind(P.matGetColor, byParams(material, "GetColor", {"System.Int32"}), "Material.GetColor");
    bind(P.matGetFloat, byParams(material, "GetFloat", {"System.Int32"}), "Material.GetFloat");
    bind(P.propertyToId, byParams(shader, "PropertyToID", {"System.String"}), "Shader.PropertyToID");
    bind(P.trGetPosition, byParams(transform, "get_position", {}), "Transform.get_position");
    bind(P.trGetParent, byParams(transform, "get_parent", {}), "Transform.get_parent");
    P.lightClass = cls(kCore, "UnityEngine", "Light");
    bind(P.lightGetIntensity, byParams(P.lightClass, "get_intensity", {}), "Light.get_intensity");
    bind(P.lightSetIntensity, byParams(P.lightClass, "set_intensity", {"System.Single"}), "Light.set_intensity");
    bind(P.lightGetColor, byParams(P.lightClass, "get_color", {}), "Light.get_color");
    bind(P.lightSetColor, byParams(P.lightClass, "set_color", {"UnityEngine.Color"}), "Light.set_color");
    if (!why.empty()) return false;
    Impl::Bind(P.rendGetBounds, R.MethodByParams(P.rendererClass, "get_bounds", {}, nullptr));  // optional (debug boxes)
    P.ready = true;
    // Music Pillar / Tile beat flash: optional (without it they still glow and their light pulses).
    {
        std::string tileWhy;
        P.musicTileClass = R.Class(kImage, "MonomiPark.SlimeRancher", "MusicTile", &tileWhy);
        if (auto o = P.musicTileClass ? R.FieldOffset(P.musicTileClass, "_effectAnimationTime", &tileWhy) : std::nullopt) P.offTileAnimTime = *o;
        if (!P.offTileAnimTime) SR2_WARN("slime_dj: lamps: Music Pillar / Tile beat flash unavailable ({})", tileWhy);
    }
    // Glowing scene ice: optional (placed lamps pulse without it).
    {
        std::string iceWhy;
        Il2CppClass* regions = R.Class(kImage, "MonomiPark.SlimeRancher.Regions", "RegionRegistry", &iceWhy);
        P.lightControlClass = R.Class(kImage, "MonomiPark.SlimeRancher.VFX.Lights", "LightControl", &iceWhy);
        P.gadgetClass = R.Class(kImage, "MonomiPark.SlimeRancher.World", "Gadget", &iceWhy);
        P.currentGroup = regions ? R.Method(regions, "get_CurrentSceneGroup", 0, &iceWhy) : nullptr;
        P.compInParent = R.MethodByParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"}, &iceWhy);
        for (Il2CppClass* c = sceneCtx; c && !P.offScRegions; c = parentOf(c))
            if (auto o = R.FieldOffset(c, "<RegionRegistry>k__BackingField")) P.offScRegions = *o;
        P.iceReady = P.lightControlClass && P.gadgetClass && P.currentGroup && P.compInParent && P.offScRegions;
        if (!P.iceReady)
            SR2_WARN("slime_dj: lamps: glowing scene ice unavailable ({})", iceWhy.empty() ? "SceneContext.RegionRegistry not found" : iceWhy);
        // Taking LightControls over (Ice Lamp, scene ice): optional; without it their lights stay unpulsed
        std::string lcWhy;
        Il2CppClass* behaviour = R.Class(kCore, "UnityEngine", "Behaviour", &lcWhy);
        if (P.lightControlClass) {
            if (auto o = R.FieldOffset(P.lightControlClass, "thisLight")) P.offLcLight = *o;
            P.lcUpdate = R.MethodByParams(P.lightControlClass, "Update", {}, &lcWhy);
        }
        if (behaviour) {
            Impl::Bind(P.behGetEnabled, R.MethodByParams(behaviour, "get_enabled", {}, &lcWhy));
            Impl::Bind(P.behSetEnabled, R.MethodByParams(behaviour, "set_enabled", {"System.Boolean"}, &lcWhy));
        }
        P.lcReady = P.lightControlClass && P.offLcLight && P.lcUpdate && P.behGetEnabled && P.behSetEnabled;
        if (!P.lcReady)
            SR2_WARN("slime_dj: lamps: LightControl lights (Ice Lamp, glowing ice) will not pulse ({})",
                     lcWhy.empty() ? "LightControl.thisLight / Behaviour.enabled not found" : lcWhy);
    }
    return true;
}

void LampPulser::Sync(const std::vector<Source>& sources, float range) {
    Impl& P = *p_;
    if (!P.ready) return;
    if (!P.ids[Impl::kBright]) {  // main thread, first use
        static constexpr const char* kNames[Impl::kProps] = {"_BrightnessMultiplier", "_IntensityDay", "_IntensityNight", "_Color",
                                                             "_ColorCore",           "_ColorEdge",    "_EmissiveColor",  "_InnerEmission",
                                                             "_LightControlColor"};
        for (int k = 0; k < Impl::kProps; ++k) P.ids[k] = P.propertyToId.fn(unity::NewString(kNames[k]), P.propertyToId.mi);
    }
    if (!Impl::Target(P.work)) {
        Impl::Free(P.work);
        if (Il2CppObject* b = P.NewBlock()) P.work = unity::NewGCHandle(b);
    }
    range = std::max(range, 1.0f);
    // Candidates: every loaded lamp at a refresh (at most every kFindSeconds; right away when speakers
    // start playing again, and within kNewSourceSeconds when a speaker joins that the last call did not
    // have: round 14 "lamps take a bit to kick in"); between refreshes only the lamps already held (their
    // speakers may move or stop). Raw pointers from Find are used in this call only.
    const ULONGLONG now = GetTickCount64();
    bool joined = false;
    for (const Source& s : sources)
        joined = joined || std::none_of(P.lastSources.begin(), P.lastSources.end(), [&](const auto& o) {
            const float dx = o.second.x - s.x, dy = o.second.y - s.y, dz = o.second.z - s.z;
            return o.first == s.channel && dx * dx + dy * dy + dz * dz < 1.0f;
        });
    P.lastSources.clear();
    for (const Source& s : sources) P.lastSources.push_back({s.channel, Vec3{s.x, s.y, s.z}});
    const ULONGLONG every = static_cast<ULONGLONG>((joined ? kNewSourceSeconds : kFindSeconds) * 1000);
    P.placed.clear();
    if (sources.empty()) {
        P.lastFind = 0;
        P.forceFind = false;
    } else if (P.forceFind || !P.lastFind || now - P.lastFind >= every) {  // forceFind: a lamp was placed (Placed)
        P.lastFind = now;
        P.forceFind = false;
        P.IceCheck();
        P.Find();
        P.loaded = P.placed.size();
    } else {
        for (const Impl::Lamp& l : P.lamps)
            if (Il2CppObject* tr = Impl::Target(l.tr)) P.placed.emplace_back(tr, l.pos);
    }
    // Which lamps are in range of a playing speaker: nearest speaker, weight by distance.
    struct Want {
        Il2CppObject* tr;
        Vec3 pos;
        const void* channel;
        float weight, d2;
    };
    std::vector<Want> want;
    for (const auto& [tr, pos] : P.placed) {
        float best = range * range;
        const void* ch = nullptr;
        for (const Source& s : sources) {
            const float dx = s.x - pos.x, dy = s.y - pos.y, dz = s.z - pos.z, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) {
                best = d2;
                ch = s.channel;
            }
        }
        if (!ch) continue;
        // Strength: every speaker of that channel in range adds its closeness x loudness; one speaker
        // close by gives a gentle pulse, several (or a loud stack) build up to the full look.
        float sum = 0;
        for (const Source& s : sources) {
            if (s.channel != ch) continue;
            const float dx = s.x - pos.x, dy = s.y - pos.y, dz = s.z - pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            sum += std::clamp((range - d) / (range * (1.0f - kFullShare)), 0.0f, 1.0f) * std::max(s.loud, 0.0f);
        }
        const float w = std::clamp(1.0f - std::exp(-kStack * sum), 0.0f, 1.0f);
        if (w <= 0.0f) continue;
        want.push_back({tr, pos, ch, w, best});
    }
    if (want.size() > kMaxLamps) {
        std::partial_sort(want.begin(), want.begin() + kMaxLamps, want.end(), [](const Want& a, const Want& b) { return a.d2 < b.d2; });
        want.resize(kMaxLamps);
    }
    // Restore and drop lamps that are gone or out of range; update the others.
    for (size_t i = 0; i < P.lamps.size();) {
        Impl::Lamp& lamp = P.lamps[i];
        Il2CppObject* tr = Impl::Target(lamp.tr);
        auto it = tr ? std::find_if(want.begin(), want.end(), [&](const Want& w) { return w.tr == tr; }) : want.end();
        if (it == want.end()) {
            P.Drop(lamp);
            P.lamps.erase(P.lamps.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (lamp.channel != it->channel) P.Restore(lamp);  // another song now: start from the lamp's own look
        lamp.channel = it->channel;
        lamp.pos = it->pos;  // a lamp moved in gadget mode is a new placement: read again at each refresh
        lamp.weight = it->weight;
        ++i;
    }
    for (const Want& w : want) {
        const bool have = std::any_of(P.lamps.begin(), P.lamps.end(), [&](const Impl::Lamp& l) { return Impl::Target(l.tr) == w.tr; });
        if (have) continue;
        Impl::Lamp lamp;
        lamp.pos = w.pos;
        lamp.channel = w.channel;
        lamp.weight = w.weight;
        lamp.tr = unity::NewGCHandle(w.tr);
        if (!lamp.tr) continue;
        if (!P.Build(lamp, w.tr)) {
            P.Drop(lamp);
            continue;
        }
        P.lamps.push_back(std::move(lamp));
    }
    if (P.lamps.size() != P.lastLogged) {
        P.lastLogged = P.lamps.size();
        SR2_INFO("slime_dj: lamps pulse at {} lamp(s) ({} loaded lamp(s), {} playing speaker(s), range {:.0f} m)", P.lamps.size(),
                 P.loaded, sources.size(), range);
    }
    P.placed.clear();
}

void LampPulser::Tick(float dt, const std::vector<Channel>& channels) {
    Impl& P = *p_;
    if (!P.ready) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    for (auto& [key, st] : P.channels) {
        st.seen = st.active = false;
        st.beatNow = 0;
    }
    for (const Channel& c : channels) {
        Impl::ChannelState& st = P.channels[c.key];
        st.seen = true;
        BeatTrack::Sample s;
        const BeatTrack::Sample* beat = nullptr;
        if (c.playing && c.track) {
            if (c.frame < st.last || st.gen != c.gen) st.last = c.frame;  // new song, seek
            st.gen = c.gen;
            if (c.track->Query(st.last, c.frame, s)) beat = &s;
            st.last = c.frame;
        }
        st.active = beat != nullptr;
        if (beat) st.energy += (beat->energy - st.energy) * (1.0f - std::exp(-dt / kEnergyTime));
        const float gain = kCalmShare + (1.0f - kCalmShare) * st.energy;
        const float level = beat ? beat->level * gain : 0.0f;
        st.env += (level - st.env) * (1.0f - std::exp(-dt / (level > st.env ? kAttack : kRelease)));
        st.pulse *= std::exp(-dt / kPulseDecay);
        if (beat && beat->beat > 0) {
            st.pulse = std::max(st.pulse, beat->beat * gain);
            st.beatNow = beat->beat * gain;
            st.hue += kHueStep * gain;
            st.hue -= std::floor(st.hue);
        }
    }
    std::erase_if(P.channels, [&](const auto& e) {
        return !e.second.seen && std::none_of(P.lamps.begin(), P.lamps.end(), [&](const Impl::Lamp& l) { return l.channel == e.first; });
    });
    for (Impl::Lamp& lamp : P.lamps) {
        auto it = P.channels.find(lamp.channel);
        Impl::ChannelState* st = it != P.channels.end() && it->second.seen ? &it->second : nullptr;
        // ease the distance weight (entering range fades in, a speaker moving closer brightens smoothly)
        lamp.shown += (lamp.weight - lamp.shown) * (1.0f - std::exp(-dt / kFadeIn));
        const bool settled = !st || (!st->active && st->env < kRest && st->pulse < kRest);
        if (settled) P.Restore(lamp);
        else P.Animate(lamp, *st);
    }
}

// Debug highlighter (modules/common/debug_marks.h): every kFindSeconds the scene ice check and a lamp
// refresh of its own (also without music: the ice of the zone is scanned once per scene group), then
// boxes for the pulsing lamps, the loaded lamps that do not pulse, and the loaded glowing ice crystals.
void LampPulser::Debug(bool want) {
    Impl& P = *p_;
    namespace dm = common::debug_marks;
    if (!P.ready) return;
    auto freeIdle = [&] {
        for (Impl::DebugLamp& d : P.debugIdle) Impl::Free(d.tr);
        P.debugIdle.clear();
    };
    if (!want) {
        if (P.debugOn) {
            P.debugOn = false;
            freeIdle();
            P.iceOn.clear();
            for (const char* k : {"slime_dj.lamps.pulsing", "slime_dj.lamps.idle", "slime_dj.lamps.ice"}) dm::Remove(k);
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!P.debugOn) {
        P.debugOn = true;
        P.debugFind = P.debugPublish = 0;
        P.debugLogged.clear();
    }
    if (!P.debugFind || now - P.debugFind >= static_cast<ULONGLONG>(kFindSeconds * 1000)) {
        P.debugFind = now;
        P.debugPublish = 0;
        P.IceCheck();
        P.Find();
        freeIdle();
        for (size_t i = 0; i < P.placedLamps && P.debugIdle.size() < kDebugMaxLamps; ++i) {
            Il2CppObject* tr = P.placed[i].first;
            Impl::DebugLamp d{P.placed[i].second, P.Measure(P.Children(tr, P.rendererClass, 48)), unity::NewGCHandle(tr)};
            if (d.tr) P.debugIdle.push_back(d);
        }
        P.placed.clear();
        P.placedLamps = 0;
        // which crystals are loaded now; bounds measured once each (a few per refresh)
        P.iceOn.assign(P.ice.size(), false);
        size_t measured = 0;
        for (size_t i = 0; i < P.ice.size(); ++i) {
            Il2CppObject* tr = Impl::Target(P.ice[i].first);
            P.iceOn[i] = tr && unity::ActiveInHierarchy(unity::GameObjectOf(tr));
            Bounds& b = P.iceBox[i];
            if (P.iceOn[i] && b.extents.x + b.extents.y + b.extents.z <= 0 && measured < kDebugBoundsPerRefresh) {
                ++measured;
                b = P.Measure(P.Children(tr, P.rendererClass, 8));
            }
        }
    }
    if (P.debugPublish && now - P.debugPublish < static_cast<ULONGLONG>(kDebugPublishSeconds * 1000)) return;
    P.debugPublish = now;
    auto box = [](const Bounds& b, const Vec3& pos, float fallback) {
        dm::Box x;
        if (b.extents.x + b.extents.y + b.extents.z > 0) {
            x.x = b.center.x, x.y = b.center.y, x.z = b.center.z;
            x.hx = std::max(b.extents.x, 0.1f), x.hy = std::max(b.extents.y, 0.1f), x.hz = std::max(b.extents.z, 0.1f);
        } else {  // not measured: a box of this half size standing on the position
            x.x = pos.x, x.y = pos.y + fallback, x.z = pos.z;
            x.hx = x.hz = fallback * 0.6f;
            x.hy = fallback;
        }
        return x;
    };
    std::vector<Il2CppObject*> pulsingObjs;
    std::vector<dm::Box> pulsing, idle, ice;
    for (const Impl::Lamp& l : P.lamps)
        if (Il2CppObject* tr = Impl::Target(l.tr)) {
            pulsingObjs.push_back(tr);
            pulsing.push_back(box(l.box, l.pos, 0.75f));
        }
    auto isPulsing = [&](Il2CppObject* o) { return std::find(pulsingObjs.begin(), pulsingObjs.end(), o) != pulsingObjs.end(); };
    for (const Impl::DebugLamp& d : P.debugIdle) {
        Il2CppObject* tr = Impl::Target(d.tr);
        if (tr && !isPulsing(tr)) idle.push_back(box(d.box, d.pos, 0.75f));
    }
    for (size_t i = 0; i < P.ice.size() && i < P.iceOn.size(); ++i) {
        if (!P.iceOn[i]) continue;
        Il2CppObject* tr = Impl::Target(P.ice[i].first);
        if (tr && !isPulsing(tr)) ice.push_back(box(P.iceBox[i], P.ice[i].second, 0.5f));
    }
    const std::string summary = std::format("{} pulsing, {} not pulsing, {} glowing ice crystal(s) loaded of {} in scene group '{}'",
                                            pulsing.size(), idle.size(), ice.size(), P.ice.size(), P.iceGroupName);
    if (summary != P.debugLogged) {
        P.debugLogged = summary;
        SR2_INFO("slime_dj: lamps: highlighter: {}", summary);
    }
    dm::Publish("slime_dj.lamps.pulsing", "pulsing lamp / music tile", kColPulsing, std::move(pulsing));
    dm::Publish("slime_dj.lamps.idle", "lamp / music tile (no playing speaker near)", kColIdle, std::move(idle));
    dm::Publish("slime_dj.lamps.ice", "glowing ice", kColIce, std::move(ice));
}

void LampPulser::Clear() {
    Impl& P = *p_;
    for (Impl::Lamp& lamp : P.lamps) P.Drop(lamp);
    if (!P.lamps.empty() || P.lastLogged != 0) SR2_INFO("slime_dj: lamps pulse stopped; lamps restored");
    P.lamps.clear();
    P.placed.clear();
    P.channels.clear();
    P.lastSources.clear();
    P.lastFind = 0;
    P.lastLogged = 0;
    P.FreeWatch();
    P.forceFind = false;
    P.recheckAt = 0;
}

}  // namespace sr2::modules::slime_dj
