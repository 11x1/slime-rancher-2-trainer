// slime_dj: custom songs from the corral Music Box ("slime speakers").
//
// Songs: .ogg / .mp3 / .wav files in <game>\sr2mods_music (created on the first start; subfolders
// are read too). library.cpp reads the folder on a background thread; stream.cpp decodes the
// playing song on another one (compiled-in stb_vorbis / dr_mp3 / dr_wav, codecs.c). The main
// thread only copies decoded PCM into the game's audio.
//
// Playback (main thread, one frame callback; one recording hook, MusicBoxUpgrader.Apply):
//   * Music boxes: the known MusicBoxUpgraders (one Object.FindObjectsByType when a song starts /
//     the tab opens, then whatever the Apply hook reports) are checked every 2 s while a song is
//     loaded or the tab is open; a box counts while MusicBoxUpgrader.MusicBox (the GameObject the
//     upgrade switches on) is active. Corrals without the upgrade are skipped.
//   * Every owned box nearby gets a speaker: a GameObject "sr2dj_Speaker" with a Unity
//     AudioSource, parented to the box's SECTR audio source (or the box itself), fully 3D with
//     the Hearing range setting (linear rolloff). All speakers play the same clip in sync
//     (started together; a box that appears later joins at the current position; a drift check
//     moves stragglers back), so walking from one corral to the next never restarts the song.
//     At most kMaxSpeakers. Objects registered through api.h (emitters.cpp) get a speaker too.
//   * The clip is a ring: AudioClip.Create(kRingChunks chunks, looping) filled with
//     AudioClip.SetData one chunk (0.5 s) at a time, ahead of the play head
//     (AudioSource.timeSamples of the first speaker, unwrapped). Memory stays the same for a
//     3-minute song and a 3-hour one. When the decoder is done the ring is padded with silence
//     and the song ends when the play head passes the last real frame.
//   * The vanilla music box tune (its SECTR_AudioSource.Volume, or a plain AudioSource's mute)
//     is silenced while a custom song plays (setting mute_vanilla) and restored when it stops,
//     is paused, or the module is turned off.
//   * Volume = setting `volume` x the game's music bus (SECTR bus whose name contains "Music",
//     its EffectiveVolume; 0 while muted). Paused while the vanilla cue's bus is paused
//     (pause menu), while the player pauses, while buffering and while no box is nearby.
//   * Queue: the tab's right column. When it runs dry the next song is a random one while
//     `shuffle` is on (default), otherwise playback stops.
//   * Corral Radio (custom_upgrades plot upgrade): a corral that owns it plays its own playlist
//     (a Channel of its own: stream, clip ring, clock, speakers) instead of the global queue. At
//     most kMaxRadioStreams radios decode at once; the others wait for a free slot (nearest first).
//   * Muffle (setting muffle_walls): every speaker in a corral gets an AudioLowPassFilter and a
//     volume drop while the listener is outside a corral with High Walls (more with the Air Net).
//   * Walls dance to the music (setting dance_walls, off by default): the decoder thread analyses
//     the song (beat.cpp: bass level + onsets per 10 ms); every frame the energy walls and the Air
//     Net of the corrals whose box plays and that are within hearing range pulse with it
//     (walls.cpp), restored when the song stops or pauses.
//
// Fault policy (like wall_markers): every game call runs inside one guarded tick or guarded
// request; a native fault marks the module faulted and stops it for the session. GC handles are
// freed only in the tick / requests (never in the hook, which only records a raw pointer).

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/paths.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/slime_dj/emitters.h"
#include "modules/slime_dj/lamps.h"
#include "modules/slime_dj/library.h"
#include "modules/slime_dj/radio_menu.h"
#include "modules/slime_dj/stream.h"
#include "modules/slime_dj/walls.h"
#include "ui/slime_dj_bridge.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace bridge = sr2::ui::slime_dj;
namespace dj = sr2::modules::slime_dj;
namespace cu = sr2::modules::custom_upgrades;
using il2cpp::api;
using il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kAudio = "UnityEngine.AudioModule";
constexpr const wchar_t* kFolderName = L"sr2mods_music";
constexpr const char* kSpeakerName = "sr2dj_Speaker";
constexpr size_t kMaxSpeakers = 12;      // music boxes
constexpr size_t kMaxEmitters = 48;      // registered emitters (api.h; every placed Boombox is one)
constexpr uint32_t kRingChunks = 16;     // 8 s ring at 0.5 s chunks
constexpr uint32_t kStartChunks = 4;     // written before the first Play (2 s)
constexpr int kWritesPerTick = 3;        // chunks copied per frame once playing
constexpr float kBoxScanInterval = 2.0f;
constexpr float kBusRetryInterval = 5.0f;
constexpr float kPublishInterval = 0.2f;
constexpr float kPerfLogInterval = 60.0f;  // box scan timing summary, at most once a minute
constexpr int kMaxExceptions = 20;
constexpr int kMaxFailuresInARow = 3;
// 3D: linear rolloff, full volume up to kFullFraction of the hearing range, silent at the range.
constexpr float kFullFraction = 0.2f;
// Amplify (round 11): each other speaker within hearing range adds kAmpVolStep volume (up to
// kAmpVolMax, +4 dB) and kAmpRangeStep range (up to kAmpRangeMax); an amplified speaker is at full
// volume only up to kAmpFullFraction of its range, so each one is heard from where it stands instead of
// one wide blob. Volume above 1 (AudioSource.volume tops out there) comes from a channel gain baked into
// the PCM written into the clip ring (peak-limited, see kPeakCeiling); the speakers' volumes are divided
// by the gain of the ring chunk being played.
constexpr float kAmpVolStep = 0.15f, kAmpVolMax = 1.6f;
constexpr float kAmpRangeStep = 0.1f, kAmpRangeMax = 1.3f;
constexpr float kAmpFullFraction = 0.1f;
constexpr float kGainStepPerChunk = 0.05f;  // the channel gain rises at most this much per 0.5 s chunk
// Round 12 ("sound is crackling a bit"): the old tanh knee above 0.89 bent every loud peak of a
// mastered song (distortion), and the gain stepped at chunk edges. Now the gain ramps across each
// chunk and never takes a sample of this chunk or the next above kPeakCeiling (look-ahead).
constexpr float kPeakCeiling = 0.97f;
// Speakers of one channel play the same samples in sync, so at the listener they add up as
// amplitudes: five speakers at full volume nearby = 5x, clipped by the mixer (crackle). Every
// channel's speakers are scaled so the estimated sum at the camera stays under kSumMax; the cut
// follows at once and lets go over kSumRelease seconds.
constexpr float kSumMax = 1.0f;
constexpr float kSumRelease = 0.6f;
// Emitter volume scales above 1 (a DJ Booth's floor speakers) come from the channel gain as well.
constexpr float kMaxGain = 2.0f;
// Speakers more than this far from the clock speaker are moved back to it (two scans in a row).
constexpr double kDriftSeconds = 0.03;
// Walls dance: the analysis is read this far behind the play head (AudioSource.timeSamples runs
// ahead of what is heard by about the output buffer).
constexpr double kDanceLagSeconds = 0.04;
// Walls dance: every audible emitter inside the corral adds this to its intensity factor, up to the max.
constexpr float kDanceSpeakerStep = 0.2f, kDanceSpeakerMax = 2.0f;
// Corral Radio: decoder streams that may run at once (the global queue not counted), radios kept.
constexpr size_t kMaxRadioStreams = 4;
constexpr size_t kMaxRadios = 32;
constexpr const char* kRadioValue = "slime_dj.radio";  // custom_upgrades per-plot value (the playlist)
constexpr size_t kMaxPlaylist = 500;
// Corral footprint (patchCorral barriers 9.5 x 9.5 m around the plot root): the listener counts as
// inside within kCorralHalf of the centre (x / z, plot axes) and kInsideBelow / kInsideAbove of its
// height; an emitter (the Music Box II prop on a corner post, a placed speaker) belongs to a corral
// within kEmitterHalf.
constexpr float kCorralHalf = 4.9f, kEmitterHalf = 5.5f, kInsideBelow = 3.0f, kInsideAbove = 8.0f;
// Muffle: low-pass cutoff (Hz) and volume heard from outside. kOpenCutoff = no filter (the component
// is switched off). Smoothed over kMuffleTau seconds (log cutoff, linear volume).
constexpr float kOpenCutoff = 22000.0f;
constexpr float kMuffleNetCut = 6500.0f, kMuffleNetVol = 0.92f;   // Air Net only
constexpr float kMuffleHighCut = 2000.0f, kMuffleHighVol = 0.8f;  // High Walls
constexpr float kMuffleBothCut = 750.0f, kMuffleBothVol = 0.6f;   // High Walls + Air Net
constexpr float kMuffleTau = 0.35f;
constexpr int kWallsHigh = 1, kWallsNet = 2;

struct Vec3 {
    float x, y, z;
};

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

struct Cfg {
    float volume = 1.0f;
    float range = 60.0f;  // hearing range, m
    bool shuffle = true;
    bool muteVanilla = true;
    bool amplify = false;
    bool danceWalls = false;
    float danceIntensity = 1.0f;
    bool muffle = true;
    bool danceLamps = true;
    float lampRange = 20.0f;  // m around a playing speaker
};

struct Channel;

// A vanilla audio source under a music box that we silenced.
struct Vanilla {
    Il2CppGCHandle h = nullptr;
    bool sectr = false;   // SECTR_AudioSource (Volume) or UnityEngine.AudioSource (mute)
    float volume = 1.0f;  // SECTR: the volume to restore
};

struct Speaker {
    Il2CppGCHandle box = nullptr;  // the MusicBox GameObject, or the registered emitter object
    Il2CppGCHandle go = nullptr;   // our GameObject
    Il2CppGCHandle src = nullptr;  // our AudioSource
    std::vector<Vanilla> vanilla;
    bool muted = false;
    unsigned clipGen = 0;  // clip generation it plays (0 = none)
    bool paused = false;
    float volume = -1.0f;
    float maxD = -1.0f;    // hearing range applied (m)
    bool ampedRange = false;  // the applied min distance is Amplify's (kAmpFullFraction)
    // emitter (api.h): owner key as registered, scales; music boxes: nullptr, 1, 1
    Il2CppObject* emitter = nullptr;
    float rangeScale = 1.0f, volScale = 1.0f;
    float boost = 1.0f;    // amplify: volume factor (other speakers in range)
    float rangeBoost = 1.0f;  // amplify: range factor
    int drift = 0;         // scans in a row it was off the clock speaker
    // Our GameObject is active in the hierarchy (checked every box scan). Unity refuses Play() on a
    // disabled AudioSource and stops it for good when its object is switched off, so a speaker that
    // is off waits (no clock, no join) and rejoins when it is on again.
    bool live = true;
    int age = 0;           // box scans since it (re)joined the song
    // World position (every box scan; Amplify and the listener sum cap).
    Vec3 pos{};
    bool posOk = false;
    // What it plays: the global queue or a Corral Radio (set every box scan by AssignSpeakers).
    Channel* ch = nullptr;
    int corral = -1;       // index into corrals_ of the last box scan (-1 = not in a corral)
    // Muffle: our AudioLowPassFilter (null = none), cutoff and volume factor now, cutoff last set.
    Il2CppGCHandle lpf = nullptr;
    float lpCut = kOpenCutoff, lpVol = 1.0f, lpSet = -1.0f;
    bool lpOn = false;
};

struct Current {
    uint64_t song = 0;
    std::string name;
    double hintSeconds = 0;  // from the library (until the decoder knows)
    std::shared_ptr<dj::Stream> stream;
    Il2CppGCHandle clip = nullptr;
    Il2CppGCHandle buf = nullptr;  // float[] chunk buffer for SetData
    unsigned clipGen = 0;
    uint32_t channels = 0, rate = 0, chunk = 0, ring = 0;
    uint64_t writeHead = 0;   // frames written into the ring (total, unwrapped)
    uint64_t realWritten = 0; // real (non-padding) frames written
    uint64_t playPos = 0;     // frames played (unwrapped timeSamples)
    uint64_t endFrame = 0;    // last real frame + 1 (0 = not known yet)
    uint64_t base = 0;        // song frame the stream started at (seek); ring positions count from it
    int32_t lastTs = 0;
    Il2CppObject* lastMaster = nullptr;  // identity only (never dereferenced)
    bool started = false, userPaused = false, buffering = false;
    std::vector<float> tmp;
    // Channel gain (Amplify, emitters louder than full volume): baked into the PCM as a ramp from
    // chunkG0 to chunkG1 over each ring chunk (by chunk index); the gain at the end of the last chunk
    // written, the one wanted. One decoded chunk is held back (pend) so the ramp can look ahead.
    std::array<float, kRingChunks> chunkG0{}, chunkG1{};
    float gain = 1.0f, wantGain = 1.0f;
    std::vector<float> pend;
    uint32_t pendFrames = 0;
    float pendPeak = 0;
    bool hasPend = false;
    // Listener sum cap: factor on every speaker of the channel (smoothed; 1 = no cut).
    float sumCap = 1.0f;
    // Decoder finished, every chunk popped and the held-back one written.
    bool Drained() const { return stream && stream->Done() && !hasPend; }
};

struct QueueItem {
    uint64_t uid = 0, song = 0;
};

// One song source: the global queue (key "") or a corral that owns the Corral Radio (key = its
// plot key, custom_upgrades PlotKey). Each has its own stream, clip ring and clock; a speaker
// plays the channel it is assigned to.
struct Channel {
    std::string key;
    std::unique_ptr<Current> cur;
    uint64_t lastSong = 0;
    int failures = 0;  // songs in a row that failed
    // radio only (kept in the plot's sidecar value kRadioValue)
    std::vector<std::string> playlist;  // song files relative to the music folder, in play order
    int index = -1;                     // playlist entry playing / played last
    bool on = false, shuffle = false;
    // radio, runtime
    bool slot = false;              // holds one of the kMaxRadioStreams decoder slots
    bool seen = false;              // its corral was found in this box scan
    Il2CppGCHandle plot = nullptr;  // its LandPlot (for the sidecar value)
    Vec3 pos{};
    bool posOk = false;
    float dist = 1e9f;              // from the camera at the last box scan
    // walls dance: analysis read up to this song frame, for this clip; this frame's sample
    uint64_t danceLast = 0;
    unsigned danceGen = 0;
    dj::BeatTrack::Sample dance{};
    bool danceHas = false;
    bool Radio() const { return !key.empty(); }
};

// A corral behind a music box found in this box scan (raw pointers: valid for this scan only).
struct Corral {
    Il2CppObject* box = nullptr;   // MusicBox GameObject
    Il2CppObject* plot = nullptr;  // its LandPlot (nullptr = unknown)
    Vec3 pos{}, right{1, 0, 0}, fwd{0, 0, 1};  // plot root position and axes
    bool posOk = false;
    int walls = 0;                 // kWallsHigh | kWallsNet
    std::string radio;             // plot key when it owns the Corral Radio
};

// Box scan timing, logged once a minute while scans ran (QueryPerformanceCounter ticks).
struct ScanPerf {
    int scans = 0, searches = 0;
    long long sum = 0, max = 0, searchMax = 0;
    float acc = 0;
    void Scan(long long t) {
        ++scans;
        sum += t;
        max = std::max(max, t);
    }
    void Search(long long t) {
        ++searches;
        searchMax = std::max(searchMax, t);
    }
};

class SlimeDj;
SlimeDj* g_self = nullptr;

// MusicBoxUpgrader.Apply(Upgrade) hook (0x98A810, 1.3.0). LandPlot.ApplyUpgrades calls Apply on every
// PlotUpgrader of the plot for every owned upgrade, on load and on each purchase, so every corral that
// owns the Music Box passes here when its plot appears or the box is bought. The detour only records
// the raw `this` (main thread); the tick pins it in the next frame. This replaces the old scene-wide
// FindObjectsByType every 2 s (8-14 ms on the ranch, user log 2026-10-24).
using ApplyFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
ApplyFn g_origApply = nullptr;
sr2::core::DetourGuard g_guardApply{"slime_dj", "MusicBoxUpgrader.Apply"};
std::atomic<bool> g_recording{false};
std::vector<Il2CppObject*> g_applied;  // main thread only, drained by the tick every frame

void ApplyDetour(Il2CppObject* self, int32_t upgrade, const MethodInfo* mi) {
    g_origApply(self, upgrade, mi);
    if (g_recording.load(std::memory_order_relaxed))
        g_guardApply.Run([&] {
            if (self && g_applied.size() < 512 && std::find(g_applied.begin(), g_applied.end(), self) == g_applied.end())
                g_applied.push_back(self);
        });
}

class SlimeDj {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string why;
        if (!Resolve(why)) return ctx.Fail(why);
        RegisterSettings();
        // Optional: without the hook every box scan searches the whole scene again (the old way).
        {
            const MethodInfo* apply = Resolver::Get().Method(upgraderClass_, "Apply", 1);
            void* target = apply ? Resolver::Get().MethodPointer(apply) : nullptr;
            std::string herr = target ? "" : "MusicBoxUpgrader.Apply(1) not found";
            hooked_ = target && ctx.hooks().Create(kModule, "slime_dj.MusicBoxUpgrader.Apply", target,
                                                   reinterpret_cast<void*>(&ApplyDetour),
                                                   reinterpret_cast<void**>(&g_origApply), &herr);
            if (!hooked_)
                SR2_WARN("slime_dj: no MusicBoxUpgrader.Apply hook ({}); music boxes are searched in the whole scene "
                         "every 2 s (a visible hitch on big ranches)", herr);
        }
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        folder_ = paths::SelfDir() / kFolderName;
        std::error_code ec;
        if (!std::filesystem::is_directory(folder_, ec)) {
            if (std::filesystem::create_directories(folder_, ec))
                SR2_INFO("slime_dj: created the music folder {}", dj::Utf8(folder_.wstring()));
            else
                SR2_WARN("slime_dj: cannot create the music folder {}: {}", dj::Utf8(folder_.wstring()), ec.message());
        }
        std::string danceWhy;
        if (!dancer_.Ready() && !dancer_.Resolve(danceWhy))
            SR2_WARN("slime_dj: 'Walls dance to the music' unavailable ({})", danceWhy);
        if (std::string lampWhy; !lamps_.Ready() && !lamps_.Resolve(lampWhy))
            SR2_WARN("slime_dj: 'Lamps pulse to the music' unavailable ({})", lampWhy);
        if (std::string menuWhy; !menu_.Ready() && !menu_.Setup(Resolver::Get(), menuWhy))
            SR2_WARN("slime_dj: Corral Radio playlist menu unavailable ({}); playlists only in the Slime DJ tab", menuWhy);
        SR2_INFO("slime_dj: resolved (AudioClip/AudioSource, MusicBoxUpgrader; SECTR source {}, cue {}, bus {}, system {}; "
                 "walls dance {})",
                 sectrSourceClass_ ? "yes" : "no", cueClass_ ? "yes" : "no", busClass_ ? "yes" : "no",
                 sectrSystemClass_ ? "yes" : "no", dancer_.Ready() ? "yes" : "no");
        return true;
    }

    void OnEnable() {
        if (dead_.load()) {
            SR2_WARN("slime_dj: stays off for this session after a native fault");
            return;
        }
        g_self = this;
        cfgDirty_.store(true);
        // Apply calls recorded before a disable may point at objects that are gone: dropped in the
        // first tick, and the next box scan searches the scene once.
        dropApplied_ = true;
        fullScan_ = true;
        g_recording.store(hooked_);
        if (!scannedOnce_) {
            scannedOnce_ = true;
            library_.Rescan(folder_);
        }
        bridge::SetHandler(&OnRequest);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
        g_recording.store(false);
        bridge::SetHandler(nullptr);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        // OnDisable runs on the main thread once the dispatcher is live: unload right away.
        if (!dead_.load() && main_thread::IsMainThread()) Guarded("disable", [this] { Shutdown(); });
    }

private:
    // ------------------------------------------------------------ resolve --
    template <class Fn>
    static bool Bind(M<Fn>& m, const MethodInfo* mi) {
        void* p = mi ? Resolver::Get().MethodPointer(mi) : nullptr;
        if (!p) return false;
        m.fn = reinterpret_cast<Fn>(p);
        m.mi = mi;
        return true;
    }

    bool Resolve(std::string& why) {
        Resolver& R = Resolver::Get();
        auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
        auto cls = [&](const char* image, const char* ns, const char* name, bool required) -> Il2CppClass* {
            std::string err;
            Il2CppClass* k = R.Class(image, ns, name, &err);
            if (!k && required && why.empty()) why = std::format("class {}.{}: {}", ns, name, err);
            return k;
        };
        auto off = [&](Il2CppClass* k, const char* field, size_t& out, bool required) {
            for (Il2CppClass* c = k; c; c = parentOf(c))
                if (auto o = R.FieldOffset(c, field)) {
                    out = *o;
                    return true;
                }
            if (required && why.empty()) why = std::format("field {} not found", field);
            return false;
        };
        auto method = [&](Il2CppClass* k, const char* name, int argc, bool required) -> const MethodInfo* {
            for (Il2CppClass* c = k; c; c = parentOf(c))
                if (const MethodInfo* m = R.Method(c, name, argc)) return m;
            if (required && why.empty()) why = std::format("method {}({}) not found", name, argc);
            return nullptr;
        };
        auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
            std::string err;
            const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
            if (!m && why.empty()) why = std::format("{}: {}", name, err);
            return m;
        };
        auto bind = [&](auto& m, const MethodInfo* mi, const char* what) {
            if (!Bind(m, mi) && why.empty()) why = std::format("{}: no native body", what);
        };

        HMODULE ga = api().module;
        objectNew_ = reinterpret_cast<ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
        arrayNew_ = reinterpret_cast<ArrayNewFn>(ga ? GetProcAddress(ga, "il2cpp_array_new") : nullptr);
        if (!objectNew_ || !arrayNew_) why = "il2cpp_object_new / il2cpp_array_new export missing";

        // game
        upgraderClass_ = cls(kImage, "", "MusicBoxUpgrader", true);
        if (upgraderClass_) off(upgraderClass_, "MusicBox", offMusicBox_, true);
        // SECTR audio (optional: without it the speakers use default 3D settings, volume 1, and
        // the vanilla tune is not silenced)
        sectrSourceClass_ = cls(kImage, "", "SECTR_AudioSource", false);
        cueClass_ = cls(kImage, "", "SECTR_AudioCue", false);
        busClass_ = cls(kImage, "", "SECTR_AudioBus", false);
        sectrSystemClass_ = cls(kImage, "", "SECTR_AudioSystem", false);
        std::string optWhy;
        auto optional = [&](bool ok, const char* what) {
            if (!ok && optWhy.empty()) optWhy = what;
            return ok;
        };
        if (sectrSourceClass_ &&
            !(optional(off(sectrSourceClass_, "Cue", offSourceCue_, false), "SECTR_AudioSource.Cue") &&
              optional(Bind(sectrGetVolume_, method(sectrSourceClass_, "get_Volume", 0, false)), "SECTR get_Volume") &&
              optional(Bind(sectrSetVolume_, method(sectrSourceClass_, "set_Volume", 1, false)), "SECTR set_Volume")))
            sectrSourceClass_ = nullptr;
        if (cueClass_ && !(optional(off(cueClass_, "MinDistance", offCueMin_, false), "cue MinDistance") &&
                           optional(off(cueClass_, "MaxDistance", offCueMax_, false), "cue MaxDistance") &&
                           optional(off(cueClass_, "Falloff", offCueFalloff_, false), "cue Falloff") &&
                           optional(off(cueClass_, "bus", offCueBus_, false), "cue bus")))
            cueClass_ = nullptr;
        if (busClass_ && !(optional(off(busClass_, "children", offBusChildren_, false), "bus children") &&
                           optional(Bind(busEffVolume_, method(busClass_, "get_EffectiveVolume", 0, false)), "bus volume") &&
                           optional(Bind(busMuted_, method(busClass_, "get_Muted", 0, false)), "bus muted") &&
                           optional(Bind(busPaused_, method(busClass_, "get_Paused", 0, false)), "bus paused")))
            busClass_ = nullptr;
        if (sectrSystemClass_ && !optional(off(sectrSystemClass_, "MasterBus", offMasterBus_, false), "MasterBus"))
            sectrSystemClass_ = nullptr;
        if (!optWhy.empty()) SR2_WARN("slime_dj: SECTR audio partly unavailable ({}); using defaults", optWhy);

        // Unity
        Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object", true);
        Il2CppClass* component = cls(kCore, "UnityEngine", "Component", true);
        Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform", true);
        gameObjectClass_ = cls(kCore, "UnityEngine", "GameObject", true);
        audioSourceClass_ = cls(kAudio, "UnityEngine", "AudioSource", true);
        audioClipClass_ = cls(kAudio, "UnityEngine", "AudioClip", true);
        singleClass_ = cls("mscorlib", "System", "Single", true);
        if (!why.empty()) return false;

        findByType_ = byParams(uobject, "FindObjectsByType",
                               {"System.Type", "UnityEngine.FindObjectsInactive", "UnityEngine.FindObjectsSortMode"});
        destroy_ = byParams(uobject, "Destroy", {"UnityEngine.Object"});
        compsInChildren_ = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
        setParent_ = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
        goCtor_ = byParams(gameObjectClass_, ".ctor", {"System.String"});
        addComponent_ = byParams(gameObjectClass_, "AddComponent", {"System.Type"});
        // optional: speaker positions for Amplify
        if (!Bind(trGetPosition_, method(transform, "get_position", 0, false)))
            SR2_WARN("slime_dj: Transform.get_position not found; Amplify does nothing");
        Bind(trGetRight_, method(transform, "get_right", 0, false));
        Bind(trGetForward_, method(transform, "get_forward", 0, false));
        // optional: Muffle (AudioLowPassFilter is in this build: managed class with set_cutoffFrequency,
        // native icalls in UnityPlayer.dll; its setter writes a one-key curve, so no distance curve
        // overrides it) and the corral's walls / net / plot
        lowPassClass_ = cls(kAudio, "UnityEngine", "AudioLowPassFilter", false);
        if (lowPassClass_ && !Bind(lpSetCutoff_, method(lowPassClass_, "set_cutoffFrequency", 1, false))) lowPassClass_ = nullptr;
        if (Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour", false))
            Bind(behSetEnabled_, method(behaviour, "set_enabled", 1, false));
        wallUpClass_ = cls(kImage, "", "WallUpgrader", false);
        if (wallUpClass_ && !off(wallUpClass_, "UpgradeWalls", offUpgradeWalls_, false)) wallUpClass_ = nullptr;
        airNetUpClass_ = cls(kImage, "", "AirNetUpgrader", false);
        if (airNetUpClass_ && !off(airNetUpClass_, "AirNets", offAirNets_, false)) airNetUpClass_ = nullptr;
        landPlotClass_ = cls(kImage, "", "LandPlot", false);
        {
            std::string err;
            compInParent_ = R.MethodByParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"}, &err);
        }
        if (!lowPassClass_ || !trGetPosition_ || !wallUpClass_ || !airNetUpClass_)
            SR2_WARN("slime_dj: 'Muffle behind High Walls' unavailable (AudioLowPassFilter {}, position {}, WallUpgrader {}, "
                     "AirNetUpgrader {})",
                     lowPassClass_ ? "ok" : "missing", trGetPosition_ ? "ok" : "missing", wallUpClass_ ? "ok" : "missing",
                     airNetUpClass_ ? "ok" : "missing");
        if (!landPlotClass_ || !compInParent_) SR2_WARN("slime_dj: Corral Radio unavailable (LandPlot / GetComponentInParent missing)");

        bind(clipCreate_,
             byParams(audioClipClass_, "Create", {"System.String", "System.Int32", "System.Int32", "System.Int32", "System.Boolean"}),
             "AudioClip.Create");
        bind(clipSetData_, byParams(audioClipClass_, "SetData", {"System.Single[]", "System.Int32"}), "AudioClip.SetData");
        auto as = [&](auto& m, const char* name, int argc) { bind(m, method(audioSourceClass_, name, argc, true), name); };
        as(srcSetClip_, "set_clip", 1);
        as(srcSetLoop_, "set_loop", 1);
        as(srcSetPlayOnAwake_, "set_playOnAwake", 1);
        as(srcSetSpatialBlend_, "set_spatialBlend", 1);
        as(srcSetRolloff_, "set_rolloffMode", 1);
        as(srcSetMinDistance_, "set_minDistance", 1);
        as(srcSetMaxDistance_, "set_maxDistance", 1);
        as(srcSetDoppler_, "set_dopplerLevel", 1);
        as(srcSetPriority_, "set_priority", 1);
        as(srcSetVolume_, "set_volume", 1);
        as(srcSetMute_, "set_mute", 1);
        as(srcGetMute_, "get_mute", 0);
        as(srcGetTimeSamples_, "get_timeSamples", 0);
        as(srcIsPlaying_, "get_isPlaying", 0);
        as(srcSetTimeSamples_, "set_timeSamples", 1);
        as(srcPause_, "Pause", 0);
        as(srcUnPause_, "UnPause", 0);
        as(srcStop_, "Stop", 0);
        bind(srcPlay_, byParams(audioSourceClass_, "Play", {}), "AudioSource.Play()");
        // optional: keeps the song clip through scene changes (Resources.UnloadUnusedAssets destroyed it)
        if (!Bind(objSetHideFlags_, method(uobject, "set_hideFlags", 1, false)))
            SR2_WARN("slime_dj: Object.set_hideFlags not found; a zone change may stop the song");
        return why.empty();
    }

    // "Lamps pulse to the music" (lamps.cpp), shown right after the walls dance settings.
    static void RegisterLampSettings() {
        using settings::Kind;
        settings::Def x;
        x.module = kModule;
        x.key = "dance_lamps";
        x.label = "Lamps pulse to the music";
        x.kind = Kind::Bool;
        x.def = 1;
        x.min = 0;
        x.max = 1;
        x.step = 1;
        x.tooltip = "Electric Pillar Lamps near a music box, DJ Booth, speaker stack or radio that plays a song "
                    "pulse their light and glow with the beat: calm in quiet parts, flashing in loud ones. They go "
                    "back to normal when the song stops or pauses.";
        settings::Register(x);
        x.key = "lamp_range";
        x.label = "Lamp range";
        x.kind = Kind::Float;
        x.def = 20.0;
        x.min = 5.0;
        x.max = 100.0;
        x.step = 5.0;
        x.parent = "dance_lamps";
        x.tooltip = "How far from a playing speaker a lamp pulses. Strongest close to the speaker, fading out "
                    "towards this distance.";
        settings::Register(x);
        settings::SetDisplay(kModule, "lamp_range", " m");
    }

    static void RegisterSettings() {
        using settings::Kind;
        auto def = [](const char* key, const char* label, Kind kind, double d, double mn, double mx, double step,
                      const char* tip) {
            settings::Def x;
            x.module = kModule;
            x.key = key;
            x.label = label;
            x.kind = kind;
            x.def = d;
            x.min = mn;
            x.max = mx;
            x.step = step;
            x.tooltip = tip;
            settings::Register(x);
        };
        def("volume", "Volume", Kind::Float, 1.0, 0.0, 1.0, 0.05,
            "Song volume at the music box, on top of the game's music volume setting.");
        def("range", "Hearing range", Kind::Float, 60.0, 10.0, 200.0, 5.0,
            "How far from a music box the song can be heard. Full volume up to a fifth of this distance, then "
            "fading out to silence at this distance.");
        def("shuffle", "Shuffle", Kind::Bool, 1, 0, 1, 1,
            "When the queue is empty, keep playing random songs from the music folder.");
        def("mute_vanilla", "Mute the vanilla tune", Kind::Bool, 1, 0, 1, 1,
            "Silences the music box's own tune while a song plays.");
        def("amplify", "Amplify with more music boxes", Kind::Bool, 0, 0, 1, 1,
            "Each other music box or speaker within hearing range of a box makes it 15 % louder (up to 60 %, also "
            "above full volume) and carry 10 % farther (up to 30 %). Amplified speakers are loudest right next to "
            "them, so you hear the song coming from every speaker around you.");
        def("dance_walls", "Walls dance to the music", Kind::Bool, 0, 0, 1, 1,
            "The energy walls of a corral whose music box plays ripple round the corral on every beat (like a "
            "shot through the wall), and they and the Air Net, if bought, glow and change colour with the song. Only corrals within hearing range. Everything goes back to "
            "normal when the song stops or pauses.");
        def("muffle_walls", "Muffle behind High Walls", Kind::Bool, 1, 0, 1, 1,
            "From outside a corral with High Walls its music sounds muffled and a bit quieter, more so with "
            "the Air Net on top (the Air Net alone muffles a little). Step inside and it is clear again.");
        {
            settings::Def x;
            x.module = kModule;
            x.key = "dance_intensity";
            x.label = "Dance intensity";
            x.kind = Kind::Float;
            x.def = 1.0;
            x.min = 0.1;
            x.max = 2.0;
            x.step = 0.05;
            x.parent = "dance_walls";
            x.tooltip = "How strongly the walls ripple, glow and change colour. Above 100 % it gets wild fast.";
            settings::Register(x);
        }
        settings::SetDisplay(kModule, "volume", "%", {}, 100);
        settings::SetDisplay(kModule, "dance_intensity", "%", {}, 100);
        settings::SetDisplay(kModule, "range", " m");
        RegisterLampSettings();
    }

    static Cfg LoadCfg() {
        Cfg c;
        c.volume = std::clamp(settings::GetFloat(kModule, "volume"), 0.0f, 1.0f);
        c.range = std::clamp(settings::GetFloat(kModule, "range"), 5.0f, 500.0f);
        if (!std::isfinite(c.range)) c.range = 60.0f;
        c.amplify = settings::GetBool(kModule, "amplify");
        c.shuffle = settings::GetBool(kModule, "shuffle");
        c.muteVanilla = settings::GetBool(kModule, "mute_vanilla");
        c.danceWalls = settings::GetBool(kModule, "dance_walls");
        c.danceIntensity = std::clamp(settings::GetFloat(kModule, "dance_intensity"), 0.0f, 2.0f);
        if (!std::isfinite(c.danceIntensity)) c.danceIntensity = 1.0f;
        c.muffle = settings::GetBool(kModule, "muffle_walls");
        c.danceLamps = settings::GetBool(kModule, "dance_lamps");
        c.lampRange = std::clamp(settings::GetFloat(kModule, "lamp_range"), 5.0f, 100.0f);
        if (!std::isfinite(c.lampRange)) c.lampRange = 20.0f;
        return c;
    }

    // ------------------------------------------------------------ guards ----
    template <class F>
    void Guarded(const char* what, F&& f) {
        if (dead_.load()) return;
        if (core::ModuleFaulted(kModule)) {
            Die("module marked faulted");
            return;
        }
        std::string err;
        core::FaultInfo fault;
        using Body = std::remove_reference_t<F>;
        const bool ok = core::GuardedCallEx([](void* p) { (*static_cast<Body*>(p))(); }, &f, &err, &fault);
        if (ok) return;
        if (fault.code != 0) {
            Die(std::format("{}: native exception {}", what, err));
            return;
        }
        if (++exceptions_ <= 3) SR2_WARN("slime_dj: {} failed: {}", what, err);
        if (exceptions_ >= kMaxExceptions) Die(std::format("{} managed exceptions, last: {}", exceptions_, err));
    }

    // Permanent stop: no further game calls this session (handles are leaked on purpose).
    void Die(const std::string& why) {
        // The menu pauses the game (UIInputLocker): never leave it open, whatever happened.
        if (menu_.IsOpen()) core::Try([this] { menu_.Close("Slime DJ stopped"); });
        menuKey_.clear();
        dead_.store(true);
        g_recording.store(false);
        bridge::SetHandler(nullptr);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        if (global_.cur && global_.cur->stream) global_.cur->stream->Stop();
        for (auto& r : radios_)
            if (r->cur && r->cur->stream) r->cur->stream->Stop();
        SR2_ERROR("slime_dj: {} - Slime DJ disabled for the rest of this session", why);
        bridge::SetStatus("Slime DJ stopped after an error (see sr2mods.log).", true);
        core::MarkModuleFaulted(kModule, why);
    }

    static void OnRequest(const bridge::Action& a) {
        if (SlimeDj* self = g_self) self->Guarded("request", [self, &a] { self->Handle(a); });
    }

    // ------------------------------------------------------ small helpers ----
    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        const bool good = err.empty();
        if (ok) *ok = good;
        if (!good) {
            if (err.rfind("native exception", 0) == 0) core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
            else SR2_WARN("slime_dj: {} failed: {}", what, err);
        }
        return r;
    }
    static Il2CppObject* Target(Il2CppGCHandle h) {
        Il2CppObject* o = h ? unity::GCHandleTarget(h) : nullptr;
        return unity::IsAlive(o) ? o : nullptr;
    }
    static void Free(Il2CppGCHandle& h) {
        if (h) unity::FreeGCHandle(h);
        h = nullptr;
    }
    void Destroy(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(destroy_, nullptr, {obj}, "Object.Destroy");
    }
    template <class T>
    static T Value(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }

    const dj::SongInfo* FindSong(uint64_t id) {
        RefreshSongIndex();
        auto it = songIndex_.find(id);
        return it == songIndex_.end() ? nullptr : &(*songs_)[it->second];
    }
    void RefreshSongIndex() {
        const unsigned gen = library_.Generation();
        if (gen == songsGen_ && songs_) return;
        songsGen_ = gen;
        songs_ = library_.Songs();
        songIndex_.clear();
        fileIndex_.clear();
        if (songs_)
            for (size_t i = 0; i < songs_->size(); ++i) {
                songIndex_.emplace((*songs_)[i].id, i);
                fileIndex_.emplace((*songs_)[i].file, i);
            }
        uiSongs_.reset();
    }
    // Radio playlists keep the file (relative to the music folder): song ids hash the full path.
    const dj::SongInfo* FindSongByFile(const std::string& file) {
        RefreshSongIndex();
        auto it = fileIndex_.find(file);
        return it == fileIndex_.end() ? nullptr : &(*songs_)[it->second];
    }

    // -------------------------------------------------------------- buses ----
    // The game's music bus (its volume follows the music volume setting) under
    // SECTR_AudioSystem.MasterBus. Looked up on demand, retried every few seconds until found.
    void FindBuses(float dt) {
        if (Target(musicBus_) || !busClass_ || !sectrSystemClass_) return;
        busRetry_ -= dt;
        if (busRetry_ > 0) return;
        busRetry_ = kBusRetryInterval;
        Il2CppObject* system = unity::FindObjectOfType(sectrSystemClass_);
        if (!unity::IsAlive(system) || !IsA(system, sectrSystemClass_)) return;
        Il2CppObject* master = unity::Field<Il2CppObject*>(system, offMasterBus_);
        if (!unity::IsAlive(master) || !IsA(master, busClass_)) return;
        std::string tree;
        Il2CppObject* music = nullptr;
        std::vector<std::pair<Il2CppObject*, int>> stack{{master, 0}};
        for (int budget = 64; !stack.empty() && budget > 0; --budget) {
            auto [bus, depth] = stack.back();
            stack.pop_back();
            const std::string name = unity::GetName(bus);
            tree += std::format("{}{}", tree.empty() ? "" : ", ", name);
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (!music && lower.find("music") != std::string::npos) music = bus;
            void* kids = unity::Field<void*>(bus, offBusChildren_);
            for (size_t i = 0, n = std::min<size_t>(unity::ListCount(kids), 32); i < n && depth < 6; ++i) {
                auto* k = static_cast<Il2CppObject*>(unity::ListAt(kids, i));
                if (unity::IsAlive(k) && IsA(k, busClass_)) stack.emplace_back(k, depth + 1);
            }
        }
        if (!masterBus_) masterBus_ = unity::NewGCHandle(master);
        if (music) {
            musicBus_ = unity::NewGCHandle(music);
            SR2_INFO("slime_dj: music bus '{}' (buses: {})", unity::GetName(music), tree);
        } else if (!busTreeLogged_) {
            busTreeLogged_ = true;
            SR2_WARN("slime_dj: no bus named like 'Music' (buses: {}); using the master bus volume", tree);
        }
    }

    float BusVolume(Il2CppObject* bus) const {
        if (!bus) return 1.0f;
        if (busMuted_.fn(bus, busMuted_.mi)) return 0.0f;
        const float v = busEffVolume_.fn(bus, busEffVolume_.mi);
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
    }

    float MusicVolume() {
        if (!busClass_) return 1.0f;
        if (Il2CppObject* b = Target(musicBus_)) return BusVolume(b);
        if (Il2CppObject* b = Target(masterBus_)) return BusVolume(b);
        return 1.0f;
    }

    bool GamePaused() {
        if (!busClass_) return false;
        Il2CppObject* b = Target(cueBus_);
        return b && busPaused_.fn(b, busPaused_.mi);
    }

    // ------------------------------------------------------------ speakers ----
    // Known MusicBoxUpgraders (ups_): one scene search (FindObjectsByType, inactive included) when a
    // song starts / the tab opens / the module is enabled, then only what the Apply hook reports (a
    // plot with upgrades loaded, an upgrade bought). The scene search walks every object of the scene
    // (8-14 ms on a big ranch), so it never runs on a timer while the hook works.
    void AddUpgrader(Il2CppObject* up) {
        if (!unity::IsAlive(up) || !IsA(up, upgraderClass_) || ups_.size() >= 256) return;
        for (Il2CppGCHandle h : ups_)
            if (unity::GCHandleTarget(h) == up) return;
        if (Il2CppGCHandle h = unity::NewGCHandle(up)) ups_.push_back(h);
    }

    // Every tick (cheap; usually nothing recorded): pins what the hook saw since the last frame.
    // True when the hook saw something (an upgrade applied: the box may have just switched on).
    bool DrainApplied() {
        if (dropApplied_) {
            dropApplied_ = false;
            g_applied.clear();
            return false;
        }
        if (g_applied.empty()) return false;
        for (Il2CppObject* up : g_applied) AddUpgrader(up);
        g_applied.clear();
        return true;
    }

    void SceneSearch() {
        const LONGLONG t0 = Qpc();
        Il2CppObject* type = unity::TypeOf(upgraderClass_);
        if (!type) return;
        int32_t inactive = 1 /* FindObjectsInactive.Include */, sort = 0 /* FindObjectsSortMode.None */;
        bool ok = false;
        Il2CppObject* arr = Call(findByType_, nullptr, {type, &inactive, &sort}, "Object.FindObjectsByType", &ok);
        if (!ok || !arr) return;
        for (size_t i = 0, n = std::min<size_t>(unity::ArrayLength(arr), 256); i < n; ++i)
            AddUpgrader(static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
        perf_.Search(Qpc() - t0);
    }

    // Owned music boxes nearby (active MusicBox GameObjects).
    std::vector<Il2CppObject*> FindBoxes() {
        std::vector<Il2CppObject*> out;
        boxUps_.clear();
        if (fullScan_ || !hooked_) {
            fullScan_ = false;
            SceneSearch();
        }
        for (size_t i = 0; i < ups_.size();) {
            Il2CppObject* up = Target(ups_[i]);
            if (!up) {  // plot demolished or its scene unloaded
                Free(ups_[i]);
                ups_.erase(ups_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
            Il2CppObject* box = unity::Field<Il2CppObject*>(up, offMusicBox_);
            if (!unity::IsAlive(box) || !IsA(box, gameObjectClass_) || !unity::ActiveInHierarchy(box)) continue;
            if (std::find(out.begin(), out.end(), box) == out.end()) {
                out.push_back(box);
                boxUps_.emplace_back(box, up);
            }
        }
        return out;
    }

    void FreeUpgraders() {
        for (Il2CppGCHandle& h : ups_) Free(h);
        ups_.clear();
        boxUps_.clear();
        fullScan_ = true;
    }

    static LONGLONG Qpc() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    std::vector<Il2CppObject*> Children(Il2CppObject* tr, Il2CppClass* k) {
        std::vector<Il2CppObject*> out;
        if (!k || !unity::IsAlive(tr)) return out;
        bool inactive = true, ok = false;
        Il2CppObject* arr = Call(compsInChildren_, tr, {unity::TypeOf(k), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? std::min<size_t>(unity::ArrayLength(arr), 32) : 0; i < n; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (unity::IsAlive(c) && IsA(c, k)) out.push_back(c);
        }
        return out;
    }

    // `e`: a registered emitter (no vanilla sources to silence, the speaker sits on the object itself);
    // nullptr: a music box.
    bool CreateSpeaker(Il2CppObject* box, Speaker& sp, const dj::emitters::Entry* e) {
        Il2CppObject* boxTr = unity::TransformOf(box);
        if (!unity::IsAlive(boxTr)) return false;
        Il2CppObject* anchor = boxTr;
        std::string found;
        if (!e) {
            for (Il2CppObject* s : Children(boxTr, sectrSourceClass_)) {
                Vanilla v;
                v.sectr = true;
                v.h = unity::NewGCHandle(s);
                if (v.h) sp.vanilla.push_back(v);
                if (anchor != boxTr) continue;
                anchor = unity::TransformOf(s);
                Il2CppObject* cue = unity::Field<Il2CppObject*>(s, offSourceCue_);
                if (cueClass_ && unity::IsAlive(cue) && IsA(cue, cueClass_)) {
                    // The cue's own range is only logged: the hearing range is the player's setting.
                    const float mn = unity::Field<float>(cue, offCueMin_), mx = unity::Field<float>(cue, offCueMax_);
                    Il2CppObject* bus = unity::Field<Il2CppObject*>(cue, offCueBus_);
                    if (!cueBus_ && unity::IsAlive(bus) && IsA(bus, busClass_)) cueBus_ = unity::NewGCHandle(bus);
                    found = std::format("cue '{}' {:.1f}-{:.1f} m {}", unity::GetName(cue), mn, mx,
                                        unity::Field<int32_t>(cue, offCueFalloff_) == 0 ? "linear" : "logarithmic");
                }
            }
            for (Il2CppObject* s : Children(boxTr, audioSourceClass_)) {
                Vanilla v;
                v.h = unity::NewGCHandle(s);
                if (v.h) sp.vanilla.push_back(v);
            }
            if (!anchor || !unity::IsAlive(anchor)) anchor = boxTr;
        } else {
            sp.emitter = e->key;
            sp.rangeScale = e->range;
            sp.volScale = e->volume;
        }

        Il2CppObject* go = objectNew_(gameObjectClass_);
        bool ok = false;
        if (go) Call(goCtor_, go, {unity::NewString(kSpeakerName)}, "GameObject..ctor", &ok);
        if (!ok || !unity::IsAlive(go)) return false;
        bool stays = false;
        Call(setParent_, unity::TransformOf(go), {anchor, &stays}, "Transform.SetParent", &ok);
        Il2CppObject* src = ok ? Call(addComponent_, go, {unity::TypeOf(audioSourceClass_)}, "AddComponent", &ok) : nullptr;
        if (!ok || !unity::IsAlive(src) || !IsA(src, audioSourceClass_)) {
            Destroy(go);
            return false;
        }
        srcSetPlayOnAwake_.fn(src, false, srcSetPlayOnAwake_.mi);
        srcSetLoop_.fn(src, true, srcSetLoop_.mi);
        srcSetSpatialBlend_.fn(src, 1.0f, srcSetSpatialBlend_.mi);
        srcSetRolloff_.fn(src, 1 /* AudioRolloffMode.Linear: silent at maxDistance */, srcSetRolloff_.mi);
        srcSetDoppler_.fn(src, 0.0f, srcSetDoppler_.mi);
        srcSetPriority_.fn(src, 32, srcSetPriority_.mi);  // 0 = most important, 128 = Unity's default
        srcSetVolume_.fn(src, 0.0f, srcSetVolume_.mi);
        // Muffle: an AudioLowPassFilter on the same object, switched off until it is needed.
        if (lowPassClass_) {
            bool lok = false;
            Il2CppObject* lpf = Call(addComponent_, go, {unity::TypeOf(lowPassClass_)}, "AddComponent(AudioLowPassFilter)", &lok);
            if (lok && unity::IsAlive(lpf) && IsA(lpf, lowPassClass_)) {
                lpSetCutoff_.fn(lpf, kOpenCutoff, lpSetCutoff_.mi);
                if (behSetEnabled_) behSetEnabled_.fn(lpf, false, behSetEnabled_.mi);
                sp.lpf = unity::NewGCHandle(lpf);
            } else if (!lowPassFailLogged_) {
                lowPassFailLogged_ = true;
                SR2_WARN("slime_dj: AudioLowPassFilter could not be added; Muffle only lowers the volume");
            }
        }
        sp.box = unity::NewGCHandle(box);
        sp.go = unity::NewGCHandle(go);
        sp.src = unity::NewGCHandle(src);
        sp.live = unity::ActiveInHierarchy(go);  // an emitter can be registered while its scene is still off
        ApplyRange(sp, src);
        if (e) {
            SR2_INFO("slime_dj: speaker on emitter '{}' (kind {}, range x{:.2f}, volume x{:.2f}{})", unity::GetName(box),
                     static_cast<int>(e->kind), e->range, e->volume, sp.live ? "" : ", switched off for now");
        } else if (!speakerLogged_) {
            speakerLogged_ = true;
            SR2_INFO("slime_dj: speaker on music box '{}' at '{}': {} vanilla source(s), {}; hearing range {:.0f} m",
                     unity::GetName(box), unity::GetName(anchor), sp.vanilla.size(),
                     found.empty() ? "no SECTR cue" : found, sp.maxD);
        } else {
            SR2_INFO("slime_dj: speaker on music box '{}' at '{}' ({} vanilla source(s))", unity::GetName(box),
                     unity::GetName(anchor), sp.vanilla.size());
        }
        return sp.box && sp.go && sp.src;
    }

    // Hearing range x the emitter's scale x Amplify's boost; set when it changed.
    void ApplyRange(Speaker& sp, Il2CppObject* src) {
        const float maxD = std::clamp(cfg_.range * sp.rangeScale * sp.rangeBoost, 2.0f, 1000.0f);
        const bool amped = sp.boost > 1.0f;
        if (std::fabs(maxD - sp.maxD) < 0.05f && amped == sp.ampedRange) return;
        srcSetMinDistance_.fn(src, std::max(0.5f, maxD * (amped ? kAmpFullFraction : kFullFraction)), srcSetMinDistance_.mi);
        srcSetMaxDistance_.fn(src, maxD, srcSetMaxDistance_.mi);
        sp.maxD = maxD;
        sp.ampedRange = amped;
    }

    void Unmute(Speaker& sp) {
        if (!sp.muted) return;
        sp.muted = false;
        for (Vanilla& v : sp.vanilla) {
            Il2CppObject* o = Target(v.h);
            if (!o) continue;
            if (v.sectr) sectrSetVolume_.fn(o, v.volume, sectrSetVolume_.mi);
            else srcSetMute_.fn(o, false, srcSetMute_.mi);
        }
    }

    void Mute(Speaker& sp) {
        if (sp.muted) return;
        sp.muted = true;
        for (Vanilla& v : sp.vanilla) {
            Il2CppObject* o = Target(v.h);
            if (!o) continue;
            if (v.sectr) {
                v.volume = sectrGetVolume_.fn(o, sectrGetVolume_.mi);
                if (!std::isfinite(v.volume)) v.volume = 1.0f;
                sectrSetVolume_.fn(o, 0.0f, sectrSetVolume_.mi);
            } else if (!srcGetMute_.fn(o, srcGetMute_.mi)) {
                srcSetMute_.fn(o, true, srcSetMute_.mi);  // (one the game muted itself is left alone)
            }
        }
    }

    void DestroySpeaker(Speaker& sp) {
        Unmute(sp);
        if (Il2CppObject* src = Target(sp.src)) srcStop_.fn(src, srcStop_.mi);
        Destroy(Target(sp.go));
        Free(sp.box);
        Free(sp.go);
        Free(sp.src);
        Free(sp.lpf);
        for (Vanilla& v : sp.vanilla) Free(v.h);
        sp.vanilla.clear();
    }

    void DestroySpeakers() {
        for (Speaker& sp : speakers_) DestroySpeaker(sp);
        speakers_.clear();
    }

    // Matches speakers to the boxes and emitters found now: gone ones lose theirs, new ones get one.
    void SyncSpeakers(const std::vector<Il2CppObject*>& boxes, const std::vector<dj::emitters::Entry>& emitters) {
        auto entryOf = [&](const Speaker& sp) -> const dj::emitters::Entry* {
            for (const auto& e : emitters)
                if (e.key == sp.emitter) return &e;
            return nullptr;
        };
        for (size_t i = 0; i < speakers_.size();) {
            Speaker& sp = speakers_[i];
            Il2CppObject* box = Target(sp.box);
            bool keep = box && Target(sp.src);
            if (keep && sp.emitter) {
                const dj::emitters::Entry* e = entryOf(sp);
                keep = e != nullptr;
                if (e) sp.rangeScale = e->range, sp.volScale = e->volume;  // re-registered with new scales
            } else if (keep) {
                keep = std::find(boxes.begin(), boxes.end(), box) != boxes.end();
            }
            if (keep) {
                ++i;
                continue;
            }
            DestroySpeaker(sp);
            speakers_.erase(speakers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        auto add = [&](Il2CppObject* obj, const dj::emitters::Entry* e) {
            Speaker sp;
            sp.ch = &global_;
            if (CreateSpeaker(obj, sp, e)) {
                speakers_.push_back(std::move(sp));
            } else {
                DestroySpeaker(sp);
                if (!speakerFailLogged_) {
                    speakerFailLogged_ = true;
                    SR2_WARN("slime_dj: could not create a speaker on '{}'", unity::GetName(obj));
                }
            }
        };
        size_t nBoxes = 0, nEmitters = 0;
        for (const Speaker& sp : speakers_) ++(sp.emitter ? nEmitters : nBoxes);
        for (Il2CppObject* box : boxes) {
            if (nBoxes >= kMaxSpeakers) break;
            const bool have = std::any_of(speakers_.begin(), speakers_.end(),
                                          [&](const Speaker& sp) { return !sp.emitter && Target(sp.box) == box; });
            if (have) continue;
            add(box, nullptr);
            ++nBoxes;
        }
        for (const auto& e : emitters) {
            if (nEmitters >= kMaxEmitters) break;
            const bool have = std::any_of(speakers_.begin(), speakers_.end(), [&](const Speaker& sp) { return sp.emitter == e.key; });
            if (have) continue;
            if (Il2CppObject* obj = Target(e.h)) add(obj, &e);
            ++nEmitters;
        }
    }

    // Every box scan. A speaker whose object is switched off (plot or actor built in a scene that is
    // not active yet, a region unloaded, the Music Box GameObject toggled by a sale + purchase) cannot
    // play: it leaves the song (clipGen 0) and TickSong lets it rejoin at the clock's position once its
    // object is on again. One that is on but stopped (switched off and on between two scans: Unity
    // stopped it and nothing plays it again) is rejoined too. Before this a stopped speaker stayed
    // silent for good, and as the clock it dragged every other speaker back to its frozen position.
    void CheckSpeakers() {
        int off = 0, restarted = 0;
        for (Speaker& sp : speakers_) {
            Il2CppObject* go = Target(sp.go);
            Il2CppObject* src = Target(sp.src);
            sp.live = go && src && unity::ActiveInHierarchy(go);
            if (!sp.live) {
                ++off;
                if (sp.clipGen && src) srcStop_.fn(src, srcStop_.mi);
                sp.clipGen = 0;
                sp.paused = false;
                sp.drift = 0;
                continue;
            }
            // (age: Play() of this frame may not report isPlaying yet; check from the next scan on)
            const Current* cc = sp.ch ? sp.ch->cur.get() : nullptr;
            if (cc && cc->started && sp.clipGen == cc->clipGen && !sp.paused && sp.age++ > 0 &&
                !srcIsPlaying_.fn(src, srcIsPlaying_.mi)) {
                sp.clipGen = 0;
                sp.drift = 0;
                ++restarted;
            }
        }
        if (restarted && (restartLogs_ += restarted) <= 10)
            SR2_INFO("slime_dj: {} speaker(s) had stopped (object switched off and on); rejoined in sync", restarted);
        const size_t nEm = static_cast<size_t>(std::count_if(speakers_.begin(), speakers_.end(), [](const Speaker& sp) { return sp.emitter; }));
        const std::array<int, 5> counts{boxes_, emitters_, static_cast<int>(speakers_.size() - nEm), static_cast<int>(nEm), off};
        if (counts != lastCounts_) {
            lastCounts_ = counts;
            SR2_INFO("slime_dj: {} music box(es), {} emitter(s) registered; speakers: {} on boxes, {} on emitters ({} switched off)",
                     counts[0], counts[1], counts[2], counts[3], counts[4]);
        }
    }

    // Every box scan: speaker positions (Amplify, the listener sum cap). Amplify: every speaker counts
    // the other audible speakers of its channel within its hearing range (round 12: switched-off booths,
    // silent Boomboxes and speakers of another radio used to count too).
    void UpdateBoost() {
        for (Speaker& sp : speakers_) {
            sp.posOk = false;
            if (!trGetPosition_ || !sp.live) continue;
            if (Il2CppObject* go = Target(sp.go); go && unity::IsAlive(unity::TransformOf(go))) {
                sp.pos = trGetPosition_.fn(unity::TransformOf(go), trGetPosition_.mi);
                sp.posOk = Finite(sp.pos);
            }
        }
        const bool on = cfg_.amplify && speakers_.size() > 1;
        for (size_t i = 0; i < speakers_.size(); ++i) {
            Speaker& a = speakers_[i];
            int n = 0;
            const float r = cfg_.range * a.rangeScale;
            for (size_t j = 0; on && a.posOk && j < speakers_.size(); ++j) {
                const Speaker& b = speakers_[j];
                if (j == i || !b.posOk || !Audible(b) || b.ch != a.ch) continue;
                const float dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
                if (dx * dx + dy * dy + dz * dz <= r * r) ++n;
            }
            a.boost = std::min(1.0f + kAmpVolStep * static_cast<float>(n), kAmpVolMax);
            a.rangeBoost = std::min(1.0f + kAmpRangeStep * static_cast<float>(n), kAmpRangeMax);
        }
    }

    // Keeps every speaker of a channel on its clock speaker's sample position. Play() starts at the
    // next audio update, so a speaker that joined later can trail by a mix buffer; one that is off by
    // more than kDriftSeconds on two box scans in a row is moved back (all readings come from the same
    // audio update, so they compare directly).
    void Resync(Channel& ch) {
        Il2CppObject* master = Master(ch);
        if (!ch.cur || !master || !ch.cur->started || ch.cur->ring == 0) return;
        const Current& c = *ch.cur;
        const int32_t ref = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
        if (ref < 0 || static_cast<uint32_t>(ref) >= c.ring) return;
        const int64_t limit = static_cast<int64_t>(kDriftSeconds * c.rate);
        for (Speaker& sp : speakers_) {
            if (sp.ch != &ch) continue;
            Il2CppObject* src = Target(sp.src);
            if (!src || src == master || !sp.live || sp.clipGen != c.clipGen || sp.paused) {
                sp.drift = 0;
                continue;
            }
            const int32_t ts = srcGetTimeSamples_.fn(src, srcGetTimeSamples_.mi);
            int64_t d = static_cast<int64_t>(ts) - ref;
            const int64_t ring = c.ring;
            if (d > ring / 2) d -= ring;
            if (d < -ring / 2) d += ring;
            if (std::llabs(d) <= limit) {
                sp.drift = 0;
                continue;
            }
            if (++sp.drift < 2) continue;
            sp.drift = 0;
            srcSetTimeSamples_.fn(src, ref, srcSetTimeSamples_.mi);
            if (++resyncs_ <= 5)
                SR2_INFO("slime_dj: speaker {:.0f} ms off the others, moved back in sync", 1000.0 * static_cast<double>(d) / c.rate);
        }
    }

    // A speaker its owner turned down to volume 0 (an emitter: a DJ Booth or Speaker Stack switched
    // off, a Boombox playing its own track). Round 12: such speakers stop instead of playing silently,
    // so they free their voice, never count for Amplify and are never the clock.
    static bool Silent(const Speaker& sp) { return sp.volScale <= 0.0f; }
    static bool Audible(const Speaker& sp) { return sp.live && !Silent(sp); }

    // --------------------------------------------------------------- songs ----
    bool AnySong() const {
        if (global_.cur) return true;
        return std::any_of(radios_.begin(), radios_.end(), [](const auto& r) { return r->cur != nullptr; });
    }
    bool HasSpeaker(const Channel& ch) const {
        return std::any_of(speakers_.begin(), speakers_.end(), [&](const Speaker& sp) { return sp.ch == &ch; });
    }
    // "Corral 2419": the last characters of the plot key (stable, short; the full key is in the tooltip).
    static std::string RadioName(const Channel& r) { return RadioName(r.key); }
    static std::string RadioName(const std::string& key) {
        std::string tail;
        for (size_t i = key.size(); i > 0 && tail.size() < 4; --i) {
            const char c = key[i - 1];
            if (std::isalnum(static_cast<unsigned char>(c))) tail.insert(tail.begin(), c);
            else if (!tail.empty()) break;
        }
        return "Corral " + (tail.empty() ? key : tail);
    }

    void StopCurrent(Channel& ch) {
        if (!ch.cur) return;
        if (ch.cur->stream) ch.cur->stream->Stop();
        // The vanilla tune stays silenced while the next song loads; TickMix / DestroySpeakers
        // bring it back when nothing plays.
        const unsigned gen = ch.cur->clipGen;
        for (Speaker& sp : speakers_) {
            if (!gen || sp.clipGen != gen) continue;
            if (Il2CppObject* src = Target(sp.src)) {
                srcStop_.fn(src, srcStop_.mi);
                srcSetClip_.fn(src, nullptr, srcSetClip_.mi);
            }
            sp.clipGen = 0;
            sp.paused = false;
        }
        Destroy(Target(ch.cur->clip));
        Free(ch.cur->clip);
        Free(ch.cur->buf);
        ch.cur.reset();
    }

    void StopAll() {
        StopCurrent(global_);
        for (auto& r : radios_) StopCurrent(*r);
    }

    bool StartSong(Channel& ch, uint64_t id) {
        const dj::SongInfo* s = FindSong(id);
        if (!s) {
            if (!ch.Radio()) bridge::SetStatus("That song is no longer in the music folder. Rescan?", true);
            return false;
        }
        if (!s->error.empty()) {
            if (!ch.Radio()) bridge::SetStatus(std::format("Can't play {}: {}", s->name, s->error), true);
            return false;
        }
        StopCurrent(ch);
        ch.cur = std::make_unique<Current>();
        ch.cur->song = s->id;
        ch.cur->name = s->name;
        ch.cur->hintSeconds = s->seconds;
        ch.cur->stream = dj::Stream::Start(s->path);
        ch.lastSong = s->id;
        if (ch.Radio()) {
            SR2_INFO("slime_dj: radio '{}' playing '{}' ({})", RadioName(ch), s->name, s->file);
        } else {
            SR2_INFO("slime_dj: playing '{}' ({})", s->name, s->file);
            bridge::SetStatus("Now playing: " + s->name, false);
        }
        return true;
    }

    // Jumps within the current song of the global queue: a new decoder from that frame and a new
    // clip; every speaker restarts on it together (TickSong), so they stay in sync. Keeps the
    // player's pause.
    void SeekTo(double seconds) {
        Channel& ch = global_;
        if (!ch.cur || !ch.cur->stream || !std::isfinite(seconds)) return;
        if (ch.cur->stream->state() != dj::Stream::State::Ready) return;  // still opening (or failed)
        const dj::SongInfo* s = FindSong(ch.cur->song);
        if (!s || !s->error.empty()) return;
        const uint32_t rate = ch.cur->stream->rate();
        if (!rate) return;
        uint64_t total = ch.cur->stream->totalFrames();
        if (!total && ch.cur->hintSeconds > 0) total = static_cast<uint64_t>(ch.cur->hintSeconds * rate);
        uint64_t frame = static_cast<uint64_t>(std::max(0.0, seconds) * rate);
        if (total) frame = std::min(frame, total > rate / 2 ? total - rate / 2 : 0);  // not past the end
        const bool paused = ch.cur->userPaused;
        const double hint = ch.cur->hintSeconds;
        StopCurrent(ch);
        ch.cur = std::make_unique<Current>();
        ch.cur->song = s->id;
        ch.cur->name = s->name;
        ch.cur->hintSeconds = hint;
        ch.cur->userPaused = paused;
        ch.cur->rate = rate;  // position shows the target while the new decoder opens
        ch.cur->base = frame;
        ch.cur->stream = dj::Stream::Start(s->path, frame);
        SR2_INFO("slime_dj: seek to {:.1f} s in '{}'", static_cast<double>(frame) / rate, s->name);
    }

    // Next song. Global queue: the queue's first, else a random one (shuffle), else stop. Radio: the
    // next playlist entry (wrapping round), a random other one with its shuffle on; `resume` starts
    // at the entry it played last (the radio coming back after a load / zone change).
    void NextSong(Channel& ch, bool resume = false) {
        StopCurrent(ch);
        if (ch.Radio()) {
            NextRadioSong(ch, resume);
            return;
        }
        while (!queue_.empty()) {
            const uint64_t id = queue_.front().song;
            queue_.pop_front();
            if (StartSong(ch, id)) return;
        }
        if (cfg_.shuffle) {
            RefreshSongIndex();
            std::vector<uint64_t> pool;
            if (songs_)
                for (const auto& s : *songs_)
                    if (s.error.empty()) pool.push_back(s.id);
            if (pool.size() > 1) std::erase(pool, ch.lastSong);
            if (!pool.empty()) {
                std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
                if (StartSong(ch, pool[pick(rng_)])) return;
            } else if (!songs_ || songs_->empty()) {
                bridge::SetStatus("No songs in the music folder yet: add .ogg, .mp3 or .wav files, then Rescan.", true);
                return;
            }
        }
        bridge::SetStatus("Queue finished.", false);
    }

    void NextRadioSong(Channel& r, bool resume) {
        const int n = static_cast<int>(r.playlist.size());
        for (int tries = 0; tries < n; ++tries) {
            int next;
            if (resume && tries == 0 && r.index >= 0 && r.index < n) {
                next = r.index;
            } else if (r.shuffle && n > 1) {
                const bool skipLast = r.index >= 0 && r.index < n;  // never the entry that just played
                std::uniform_int_distribution<int> pick(0, skipLast ? n - 2 : n - 1);
                next = pick(rng_);
                if (skipLast && next >= r.index) ++next;
            } else {
                next = r.index < 0 ? 0 : (r.index + 1) % n;
            }
            r.index = next;
            const dj::SongInfo* s = FindSongByFile(r.playlist[static_cast<size_t>(next)]);
            if (s && StartSong(r, s->id)) {
                SaveRadio(r);
                return;
            }
        }
        r.on = false;
        r.slot = false;
        SaveRadio(r);
        SR2_INFO("slime_dj: radio '{}' stopped: {}", RadioName(r), n ? "no playable song in its playlist" : "its playlist is empty");
        bridge::SetStatus(std::format("{}: {}", RadioName(r), n ? "no playable song in its playlist (Rescan?)" : "the playlist is empty"), true);
    }

    void SongFailed(Channel& ch, const std::string& why) {
        const std::string name = ch.cur ? ch.cur->name : std::string("?");
        SR2_WARN("slime_dj: {}'{}' failed: {}", ch.Radio() ? std::format("radio '{}': ", RadioName(ch)) : std::string(), name, why);
        bridge::SetStatus(std::format("{}Can't play {}: {}", ch.Radio() ? RadioName(ch) + ": " : std::string(), name, why), true);
        if (++ch.failures >= kMaxFailuresInARow) {
            ch.failures = 0;
            StopCurrent(ch);
            if (ch.Radio()) {  // else AllocateSlots starts it again at the next scan
                ch.on = false;
                ch.slot = false;
                SaveRadio(ch);
            }
            return;
        }
        NextSong(ch);
    }

    bool CreateClip(Channel& ch) {
        Current& c = *ch.cur;
        c.channels = c.stream->channels();
        c.rate = c.stream->rate();
        c.chunk = c.stream->chunkFrames();
        c.ring = c.chunk * kRingChunks;
        Il2CppObject* clip = clipCreate_.fn(unity::NewString(std::format("sr2dj_{}", c.song)), static_cast<int32_t>(c.ring),
                                            static_cast<int32_t>(c.channels), static_cast<int32_t>(c.rate), false,
                                            clipCreate_.mi);
        if (!unity::IsAlive(clip) || !IsA(clip, audioClipClass_)) return false;
        // HideFlags.DontUnloadUnusedAsset: a zone change (Resources.UnloadUnusedAssets) destroyed the clip
        // while no speaker held it ("the AudioClip was destroyed", user log 2026-10-24). StopCurrent still
        // destroys it.
        if (objSetHideFlags_) objSetHideFlags_.fn(clip, 32, objSetHideFlags_.mi);
        c.clip = unity::NewGCHandle(clip);
        Il2CppObject* buf = arrayNew_(singleClass_, static_cast<uintptr_t>(c.chunk) * c.channels);
        if (!buf || unity::ArrayLength(buf) != static_cast<size_t>(c.chunk) * c.channels) return false;
        c.buf = unity::NewGCHandle(buf);
        c.clipGen = ++clipGen_;
        return c.clip && c.buf;
    }

    // Copies chunks into the ring ahead of the play head (never into the chunk being played).
    // One decoded chunk is held back so the channel gain can look one chunk ahead: the gain ramps
    // from where the last chunk ended to a value that keeps this chunk and the next one under
    // kPeakCeiling, so no sample is bent or clipped and there is no step at a chunk edge.
    // False when the song was dropped (ch.cur replaced or gone).
    bool Fill(Channel& ch, int maxWrites) {
        Current& c = *ch.cur;
        Il2CppObject* clip = Target(c.clip);
        auto* buf = unity::GCHandleTarget(c.buf);
        if (!clip || !buf) {
            SongFailed(ch, "the AudioClip was destroyed");
            return false;
        }
        float* dst = unity::ArrayData<float>(buf);
        const size_t n = static_cast<size_t>(c.chunk) * c.channels;
        const uint64_t playChunkStart = c.playPos / c.chunk * c.chunk;
        for (int i = 0; i < maxWrites && c.writeHead + c.chunk <= playChunkStart + c.ring; ++i) {
            if (!c.hasPend) {
                if (c.stream->Pop(c.pend, c.pendFrames) && c.pend.size() == n) {
                    c.hasPend = true;
                    c.pendPeak = Peak(c.pend);
                } else if (!c.stream->Done()) {
                    break;  // the decoder is behind
                }
            }
            const size_t slot = (c.writeHead / c.chunk) % kRingChunks;
            if (c.hasPend) {
                uint32_t frames = 0;
                float nextPeak = 0;
                bool next = false;
                if (c.stream->Pop(c.tmp, frames) && c.tmp.size() == n) next = true;
                else if (!c.stream->Done()) break;  // wait for the look-ahead chunk (or the end)
                const float g0 = c.gain;
                float g1 = std::min(c.wantGain, g0 + kGainStepPerChunk);
                if (c.pendPeak > 0) g1 = std::min(g1, kPeakCeiling / c.pendPeak);
                if (next) {
                    nextPeak = Peak(c.tmp);
                    if (nextPeak > 0) g1 = std::min(g1, kPeakCeiling / nextPeak);
                }
                g1 = std::max(g1, 1.0f);
                if (g0 > 1.0005f || g1 > 1.0005f) ApplyRamp(c.pend, c.channels, g0, g1);
                std::memcpy(dst, c.pend.data(), n * sizeof(float));
                c.realWritten += c.pendFrames;
                c.chunkG0[slot] = g0;
                c.chunkG1[slot] = g1;
                c.gain = g1;
                if (next) {
                    std::swap(c.pend, c.tmp);
                    c.pendFrames = frames;
                    c.pendPeak = nextPeak;
                } else {
                    c.hasPend = false;
                }
            } else {  // decoder done and everything written
                if (!c.endFrame) c.endFrame = std::max<uint64_t>(c.realWritten, 1);
                std::memset(dst, 0, n * sizeof(float));  // silence after the end
                c.chunkG0[slot] = c.chunkG1[slot] = c.gain;
            }
            const int32_t offset = static_cast<int32_t>(c.writeHead % c.ring);
            if (!clipSetData_.fn(clip, static_cast<Il2CppArray*>(static_cast<void*>(buf)), offset, clipSetData_.mi)) {
                SongFailed(ch, "AudioClip.SetData refused the data");
                return false;
            }
            c.writeHead += c.chunk;
        }
        return true;
    }

    static float Peak(const std::vector<float>& pcm) {
        float m = 0;
        for (float x : pcm) m = std::max(m, std::fabs(x));
        return std::isfinite(m) ? m : 1.0f;
    }

    // The channel gain over one chunk: linear from g0 at its first frame to g1 at its end.
    static void ApplyRamp(std::vector<float>& pcm, uint32_t channels, float g0, float g1) {
        const size_t frames = channels ? pcm.size() / channels : 0;
        if (!frames) return;
        const float step = (g1 - g0) / static_cast<float>(frames);
        float g = g0;
        for (size_t f = 0; f < frames; ++f, g += step)
            for (uint32_t k = 0; k < channels; ++k) pcm[f * channels + k] *= g;
    }

    // The channel gain baked into the sample now playing (the ramp of its chunk at the play head).
    static float GainAt(const Current* c) {
        if (!c || !c->chunk || !c->started) return 1.0f;
        const size_t slot = (c->playPos / c->chunk) % kRingChunks;
        const float t = static_cast<float>(c->playPos % c->chunk) / static_cast<float>(c->chunk);
        const float g = c->chunkG0[slot] + (c->chunkG1[slot] - c->chunkG0[slot]) * t;
        return std::isfinite(g) ? std::max(1.0f, g) : 1.0f;
    }

    // The clock of a channel: its first speaker that plays the song (or is paused with the others).
    // One that stopped since the last box scan (its object switched off) is passed over, so the play
    // head never freezes on it.
    Il2CppObject* Master(Channel& ch) {
        if (!ch.cur) return nullptr;
        for (Speaker& sp : speakers_)
            if (sp.ch == &ch && sp.live && sp.clipGen == ch.cur->clipGen)
                if (Il2CppObject* src = Target(sp.src); src && (sp.paused || srcIsPlaying_.fn(src, srcIsPlaying_.mi))) return src;
        return nullptr;
    }

    void TickSong(Channel& ch) {
        if (!ch.cur) return;
        Current& c = *ch.cur;
        if (c.stream->state() == dj::Stream::State::Opening) return;
        if (c.stream->state() == dj::Stream::State::Failed) {
            SongFailed(ch, c.stream->error());
            return;
        }
        if (!c.clip && !CreateClip(ch)) {
            SongFailed(ch, "AudioClip.Create failed");
            return;
        }
        // Play head.
        Il2CppObject* master = Master(ch);
        if (c.started && master) {
            const int32_t ts = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
            // Another speaker taking over continues from the same position: every speaker plays in
            // sync (and stopped ones are never the clock), so the delta stays valid across them.
            c.lastMaster = master;
            if (ts >= 0 && static_cast<uint32_t>(ts) < c.ring) {
                const uint32_t delta = (static_cast<uint32_t>(ts) + c.ring - static_cast<uint32_t>(c.lastTs)) % c.ring;
                if (delta < c.ring - c.chunk) c.playPos += delta;  // a tiny step back = another speaker's jitter
            }
            c.lastTs = ts;
        }
        if (c.endFrame && c.playPos >= c.endFrame) {
            ch.failures = 0;
            NextSong(ch);
            return;
        }
        if (!Fill(ch, c.started ? kWritesPerTick : static_cast<int>(kRingChunks))) return;
        // Buffering: the decoder fell behind the play head.
        const uint64_t ahead = c.writeHead > c.playPos ? c.writeHead - c.playPos : 0;
        if (!c.Drained()) {
            if (c.started && ahead < c.chunk / 4) c.buffering = true;
            else if (c.buffering && (ahead >= 2ull * c.chunk || c.stream->Queued() >= 2)) c.buffering = false;
        } else {
            c.buffering = false;
        }
        // First start once enough is written.
        const bool ready = c.writeHead >= static_cast<uint64_t>(kStartChunks) * c.chunk || c.Drained();
        Il2CppObject* clip = Target(c.clip);
        const bool wantPause = c.userPaused || c.buffering || GamePaused() || !HasSpeaker(ch);
        // Where a (re)joining speaker starts: the playing speakers' position, else where the song
        // stopped when the last box went away (the ring still holds what follows).
        int32_t joinAt = static_cast<int32_t>(c.playPos % c.ring);
        if (master) joinAt = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
        for (Speaker& sp : speakers_) {
            if (sp.ch != &ch) continue;
            Il2CppObject* src = Target(sp.src);
            if (!src || !clip || !sp.live) continue;  // switched off: rejoins when it is on again
            if (Silent(sp)) {  // its owner set it to volume 0 (a booth switched off): it does not play at all
                if (sp.clipGen) srcStop_.fn(src, srcStop_.mi);
                sp.clipGen = 0;
                sp.paused = false;
                sp.drift = 0;
                continue;
            }
            if (sp.clipGen != c.clipGen) {
                if (!ready) continue;
                srcSetClip_.fn(src, clip, srcSetClip_.mi);
                srcSetLoop_.fn(src, true, srcSetLoop_.mi);
                srcSetTimeSamples_.fn(src, joinAt, srcSetTimeSamples_.mi);
                srcPlay_.fn(src, srcPlay_.mi);
                sp.clipGen = c.clipGen;
                sp.paused = false;
                sp.age = 0;
                if (!master) {  // first speaker (song start, or back after every box was gone)
                    master = src;
                    c.started = true;
                    c.lastTs = joinAt;
                    c.lastMaster = src;
                }
            }
            if (wantPause != sp.paused) {
                if (wantPause) srcPause_.fn(src, srcPause_.mi);
                else srcUnPause_.fn(src, srcUnPause_.mi);
                sp.paused = wantPause;
            }
        }
    }

    void TickMix(float dt) {
        const float vol = std::clamp(cfg_.volume * MusicVolume(), 0.0f, 2.0f);
        // What a speaker wants to sound like (before the sum cap): AudioSource.volume tops out at 1, so a
        // channel whose loudest speaker wants more gets that gain baked into the PCM (Fill) and every
        // speaker divides it out again, by the gain at the play head.
        auto wantOf = [&](const Speaker& sp) { return vol * sp.volScale * sp.boost * sp.lpVol; };
        // Listener sum cap: the speakers of one channel play the same samples in sync, so at the camera
        // they add up as amplitudes (linear rolloff, as Unity applies it; panning ignored = safe side).
        Vec3 cam{};
        const bool haveCam = !speakers_.empty() && CameraPos(cam);
        const float step = std::isfinite(dt) && dt > 0 ? std::min(dt, 0.5f) : 0.0f;
        auto heard = [&](const Speaker& sp) {
            if (!haveCam || !sp.posOk || sp.maxD <= 0) return 0.0f;
            const float dx = sp.pos.x - cam.x, dy = sp.pos.y - cam.y, dz = sp.pos.z - cam.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float minD = std::max(0.5f, sp.maxD * (sp.ampedRange ? kAmpFullFraction : kFullFraction));
            if (d <= minD) return 1.0f;
            if (d >= sp.maxD) return 0.0f;
            return (sp.maxD - d) / (sp.maxD - minD);
        };
        auto setWant = [&](Channel& ch) {
            if (!ch.cur) return;
            Current& c = *ch.cur;
            float want = 1.0f, sum = 0.0f;
            for (const Speaker& sp : speakers_) {
                if (sp.ch != &ch || !Audible(sp)) continue;
                const float w = wantOf(sp);
                want = std::max(want, w);
                sum += w * heard(sp);
            }
            const float cap = sum > kSumMax ? kSumMax / sum : 1.0f;
            if (cap < c.sumCap) c.sumCap = cap;
            else c.sumCap += (cap - c.sumCap) * (1.0f - std::exp(-step / kSumRelease));
            if (!std::isfinite(c.sumCap) || c.sumCap <= 0) c.sumCap = 1.0f;
            c.wantGain = std::clamp(want * c.sumCap, 1.0f, kMaxGain);
            if (c.sumCap < 0.999f && !sumCapLogged_) {
                sumCapLogged_ = true;
                SR2_INFO("slime_dj: {} speaker(s) of '{}' add up to {:.2f}x full scale here; turned down to stay clean (logged once)",
                         std::count_if(speakers_.begin(), speakers_.end(), [&](const Speaker& sp) { return sp.ch == &ch && Audible(sp); }),
                         ch.Radio() ? RadioName(ch) : std::string("the queue"), sum);
            }
        };
        setWant(global_);
        for (auto& r : radios_) setWant(*r);
        for (Speaker& sp : speakers_) {
            const Current* cc = sp.ch ? sp.ch->cur.get() : nullptr;
            const float cap = cc ? cc->sumCap : 1.0f;
            const float v = Silent(sp) ? 0.0f : std::clamp(wantOf(sp) * cap / GainAt(cc), 0.0f, 1.0f);
            Il2CppObject* src = Target(sp.src);
            if (src && std::fabs(sp.volume - v) > 0.002f) {
                srcSetVolume_.fn(src, v, srcSetVolume_.mi);
                sp.volume = v;
            }
            if (src) ApplyRange(sp, src);
            // The vanilla tune is silenced while the speaker's own channel has a song (a radio corral
            // whose radio is off or waiting for a slot keeps its tune).
            if (cc && !cc->userPaused && cfg_.muteVanilla) Mute(sp);
            else Unmute(sp);
        }
    }

    // ------------------------------------------------------------ corrals ----
    static bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    bool CameraPos(Vec3& out) {
        if (!trGetPosition_) return false;
        Il2CppObject* c = unity::Camera_main();
        Il2CppObject* tr = c ? unity::TransformOf(c) : nullptr;
        if (!unity::IsAlive(tr)) return false;
        out = trGetPosition_.fn(tr, trGetPosition_.mi);
        return Finite(out);
    }

    // `p` within `half` of the corral's centre along its own x / z axes and within its height band.
    static bool Inside(const Corral& c, const Vec3& p, float half) {
        if (!c.posOk) return false;
        const float dx = p.x - c.pos.x, dy = p.y - c.pos.y, dz = p.z - c.pos.z;
        const float lx = dx * c.right.x + dy * c.right.y + dz * c.right.z;
        const float lz = dx * c.fwd.x + dy * c.fwd.y + dz * c.fwd.z;
        return std::fabs(lx) <= half && std::fabs(lz) <= half && dy >= -kInsideBelow && dy <= kInsideAbove;
    }

    // High Walls: WallUpgrader.UpgradeWalls active; Air Net: one of AirNetUpgrader.AirNets active (the
    // owned net for the current walls). Both components sit on the plot root with MusicBoxUpgrader.
    int WallsOf(Il2CppObject* up) {
        int w = 0;
        if (wallUpClass_)
            if (Il2CppObject* wu = unity::GetComponent(up, wallUpClass_); unity::IsAlive(wu) && IsA(wu, wallUpClass_)) {
                Il2CppObject* hi = unity::Field<Il2CppObject*>(wu, offUpgradeWalls_);
                if (unity::IsAlive(hi) && IsA(hi, gameObjectClass_) && unity::ActiveInHierarchy(hi)) w |= kWallsHigh;
            }
        if (airNetUpClass_)
            if (Il2CppObject* an = unity::GetComponent(up, airNetUpClass_); unity::IsAlive(an) && IsA(an, airNetUpClass_)) {
                void* nets = unity::Field<void*>(an, offAirNets_);
                for (size_t i = 0, n = nets ? std::min<size_t>(unity::ArrayLength(nets), 4) : 0; i < n; ++i) {
                    auto* go = static_cast<Il2CppObject*>(unity::ArrayAt(nets, i));
                    if (unity::IsAlive(go) && IsA(go, gameObjectClass_) && unity::ActiveInHierarchy(go)) w |= kWallsNet;
                }
            }
        return w;
    }

    // Every box scan: the corral behind every box found (plot position and axes for the muffle, its
    // walls / net, and whether it owns the Corral Radio).
    void BuildCorrals() {
        corrals_.clear();
        const bool radios = cu::Active() && landPlotClass_ && compInParent_;
        for (const auto& [box, up] : boxUps_) {
            if (!unity::IsAlive(up)) continue;
            Corral c;
            c.box = box;
            Il2CppObject* tr = unity::TransformOf(up);
            if (trGetPosition_ && unity::IsAlive(tr)) {
                c.pos = trGetPosition_.fn(tr, trGetPosition_.mi);
                if (trGetRight_ && trGetForward_) {
                    c.right = trGetRight_.fn(tr, trGetRight_.mi);
                    c.fwd = trGetForward_.fn(tr, trGetForward_.mi);
                }
                c.posOk = Finite(c.pos) && Finite(c.right) && Finite(c.fwd);
            }
            c.walls = WallsOf(up);
            if (radios) {
                bool inactive = true, ok = false;
                Il2CppObject* plot = Call(compInParent_, up, {unity::TypeOf(landPlotClass_), &inactive}, "GetComponentInParent(LandPlot)", &ok);
                if (ok && unity::IsAlive(plot) && IsA(plot, landPlotClass_)) {
                    c.plot = plot;
                    if (cu::Owns(plot, cu::kCorralRadio)) c.radio = cu::PlotKey(plot);
                }
            }
            corrals_.push_back(std::move(c));
        }
        int walled = 0;
        for (const Corral& c : corrals_) walled += c.walls ? 1 : 0;
        const std::array<int, 2> counts{static_cast<int>(corrals_.size()), walled};
        if (counts != lastCorralCounts_) {
            lastCorralCounts_ = counts;
            SR2_INFO("slime_dj: {} corral(s) with a music box, {} with High Walls / Air Net (muffled from outside{})", counts[0],
                     counts[1], cfg_.muffle ? "" : ": setting off");
        }
    }

    Channel* FindRadio(const std::string& key) {
        for (auto& r : radios_)
            if (r->key == key) return r.get();
        return nullptr;
    }

    // The radio's playlist and state from its plot's sidecar value: lines "on=0|1", "shuffle=0|1",
    // "index=N", then one "song=<file>" per entry (files relative to the music folder).
    void LoadRadio(Channel& r, Il2CppObject* plot) {
        const std::string v = cu::PlotValue(plot, kRadioValue);
        size_t pos = 0;
        while (pos < v.size()) {
            size_t end = v.find('\n', pos);
            if (end == std::string::npos) end = v.size();
            const std::string line = v.substr(pos, end - pos);
            pos = end + 1;
            if (line.rfind("song=", 0) == 0 && line.size() > 5 && r.playlist.size() < kMaxPlaylist) r.playlist.push_back(line.substr(5));
            else if (line == "on=1") r.on = true;
            else if (line == "shuffle=1") r.shuffle = true;
            else if (line.rfind("index=", 0) == 0) r.index = std::atoi(line.c_str() + 6);
        }
        if (r.index < -1 || r.index >= static_cast<int>(r.playlist.size())) r.index = -1;
    }

    void SaveRadio(Channel& r) {
        Il2CppObject* plot = Target(r.plot);
        if (!plot) return;
        std::string v = std::format("v=1\non={}\nshuffle={}\nindex={}\n", r.on ? 1 : 0, r.shuffle ? 1 : 0, r.index);
        for (const std::string& f : r.playlist) v += "song=" + f + "\n";
        if (!cu::SetPlotValue(plot, kRadioValue, v) && !radioSaveWarned_) {
            radioSaveWarned_ = true;
            SR2_WARN("slime_dj: could not store the playlist of radio '{}' (custom upgrades off or no save loaded)", RadioName(r));
        }
    }

    void DropRadio(size_t i) {
        Channel& r = *radios_[i];
        StopCurrent(r);
        for (Speaker& sp : speakers_) {
            if (sp.ch != &r) continue;
            sp.ch = &global_;
            sp.clipGen = 0;
            sp.paused = false;
            sp.drift = 0;
        }
        Free(r.plot);
        SR2_INFO("slime_dj: radio '{}' gone (corral not loaded, radio sold, or custom upgrades off)", RadioName(r));
        radios_.erase(radios_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    void DropRadios() {
        while (!radios_.empty()) DropRadio(radios_.size() - 1);
    }

    // Box scan: one channel per corral that owns the Corral Radio and is loaded now; its playlist is
    // read from the sidecar when it appears (another save may be loaded since it was last seen).
    void SyncRadios() {
        for (auto& r : radios_) r->seen = false;
        for (const Corral& c : corrals_) {
            if (c.radio.empty() || !c.plot) continue;
            Channel* r = FindRadio(c.radio);
            if (!r) {
                if (radios_.size() >= kMaxRadios) continue;
                auto ch = std::make_unique<Channel>();
                ch->key = c.radio;
                LoadRadio(*ch, c.plot);
                r = ch.get();
                radios_.push_back(std::move(ch));
                SR2_INFO("slime_dj: radio '{}' (plot '{}'): {} song(s) in its playlist, {}{}", RadioName(*r), r->key,
                         r->playlist.size(), r->on ? "on" : "off", r->shuffle ? ", shuffle" : "");
            }
            if (r->seen) continue;
            r->seen = true;
            if (Target(r->plot) != c.plot) {
                Free(r->plot);
                r->plot = unity::NewGCHandle(c.plot);
            }
            r->pos = c.pos;
            r->posOk = c.posOk;
        }
        for (size_t i = radios_.size(); i > 0; --i)
            if (!radios_[i - 1]->seen) DropRadio(i - 1);
    }

    // Box scan: decoder slots for the radios that are on (kMaxRadioStreams at once). A radio keeps its
    // slot while it is on and loaded; free slots go to the nearest waiting ones. A radio with a slot
    // and no song starts its playlist where it left off.
    void AllocateSlots() {
        Vec3 cam{};
        const bool haveCam = CameraPos(cam);
        size_t used = 0;
        for (auto& r : radios_) {
            const bool want = r->on && !r->playlist.empty();
            if (!want && (r->slot || r->cur)) {
                StopCurrent(*r);
                r->slot = false;
            }
            if (r->slot) ++used;
            r->dist = 1e9f;
            if (haveCam && r->posOk) {
                const float dx = r->pos.x - cam.x, dy = r->pos.y - cam.y, dz = r->pos.z - cam.z;
                r->dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
        while (used < kMaxRadioStreams) {
            Channel* best = nullptr;
            for (auto& r : radios_)
                if (r->on && !r->playlist.empty() && !r->slot && (!best || r->dist < best->dist)) best = r.get();
            if (!best) break;
            best->slot = true;
            ++used;
        }
        RefreshSongIndex();
        if (!songs_) return;  // the music folder is still being read: start later
        for (auto& r : radios_)
            if (r->slot && !r->cur) {
                r->failures = 0;
                NextSong(*r, true);
            }
    }

    // Box scan: which corral every speaker is in (a box's own corral; an emitter by position) and so
    // which channel it plays: the corral's radio, else the global queue. A speaker that changes
    // channel leaves its clip and joins the other channel's song in TickSong.
    void AssignSpeakers() {
        int moved = 0;
        for (Speaker& sp : speakers_) {
            sp.corral = -1;
            Il2CppObject* box = Target(sp.box);
            if (!sp.emitter) {
                for (size_t i = 0; i < corrals_.size(); ++i)
                    if (box && corrals_[i].box == box) sp.corral = static_cast<int>(i);
            } else if (trGetPosition_) {
                Il2CppObject* go = Target(sp.go);
                Il2CppObject* tr = go ? unity::TransformOf(go) : nullptr;
                if (unity::IsAlive(tr)) {
                    const Vec3 p = trGetPosition_.fn(tr, trGetPosition_.mi);
                    for (size_t i = 0; Finite(p) && i < corrals_.size(); ++i)
                        if (Inside(corrals_[i], p, kEmitterHalf)) {
                            sp.corral = static_cast<int>(i);
                            break;
                        }
                }
            }
            Channel* want = &global_;
            if (sp.corral >= 0 && !corrals_[static_cast<size_t>(sp.corral)].radio.empty())
                if (Channel* r = FindRadio(corrals_[static_cast<size_t>(sp.corral)].radio)) want = r;
            if (sp.ch == want) continue;
            if (sp.clipGen)
                if (Il2CppObject* src = Target(sp.src)) {
                    srcStop_.fn(src, srcStop_.mi);
                    srcSetClip_.fn(src, nullptr, srcSetClip_.mi);
                }
            sp.ch = want;
            sp.clipGen = 0;
            sp.paused = false;
            sp.drift = 0;
            ++moved;
        }
        if (moved && (channelLogs_ += moved) <= 20)
            SR2_INFO("slime_dj: {} speaker(s) switched between the global queue and a corral radio", moved);
    }

    // Every frame: the low-pass cutoff and volume factor of every speaker in a corral with High Walls /
    // Air Net, muffled while the listener (main camera) is outside that corral, clear inside.
    // Smoothed, so walking in or out fades over ~1 s.
    void TickMuffle(float dt) {
        if (speakers_.empty()) return;
        auto walled = [&](const Speaker& sp) {
            return sp.corral >= 0 && static_cast<size_t>(sp.corral) < corrals_.size() && corrals_[static_cast<size_t>(sp.corral)].walls;
        };
        Vec3 cam{};
        const bool any = cfg_.muffle && std::any_of(speakers_.begin(), speakers_.end(), walled);
        const bool haveCam = any && CameraPos(cam);
        const float step = std::isfinite(dt) && dt > 0 ? std::min(dt, 0.5f) : 0.0f;
        const float k = 1.0f - std::exp(-step / kMuffleTau);
        for (Speaker& sp : speakers_) {
            float cut = kOpenCutoff, vol = 1.0f;
            if (haveCam && walled(sp)) {
                const Corral& c = corrals_[static_cast<size_t>(sp.corral)];
                if (!Inside(c, cam, kCorralHalf)) {
                    const bool high = c.walls & kWallsHigh, net = c.walls & kWallsNet;
                    cut = high && net ? kMuffleBothCut : high ? kMuffleHighCut : kMuffleNetCut;
                    vol = high && net ? kMuffleBothVol : high ? kMuffleHighVol : kMuffleNetVol;
                }
            }
            if (!(sp.lpCut > 0) || !std::isfinite(sp.lpCut)) sp.lpCut = kOpenCutoff;
            sp.lpCut = std::exp(std::log(sp.lpCut) + (std::log(cut) - std::log(sp.lpCut)) * k);
            sp.lpVol += (vol - sp.lpVol) * k;
            if (std::fabs(sp.lpCut - cut) < cut * 0.005f) sp.lpCut = cut;
            if (std::fabs(sp.lpVol - vol) < 0.002f) sp.lpVol = vol;
            ApplyLowPass(sp);
        }
    }

    void ApplyLowPass(Speaker& sp) {
        Il2CppObject* f = Target(sp.lpf);
        if (!f) return;
        const bool on = sp.lpCut < kOpenCutoff * 0.95f;
        if (on && (sp.lpSet < 0 || std::fabs(sp.lpCut - sp.lpSet) > sp.lpSet * 0.01f)) {
            lpSetCutoff_.fn(f, sp.lpCut, lpSetCutoff_.mi);
            sp.lpSet = sp.lpCut;
        }
        if (on != sp.lpOn) {
            if (!on) {
                lpSetCutoff_.fn(f, kOpenCutoff, lpSetCutoff_.mi);
                sp.lpSet = kOpenCutoff;
            }
            if (behSetEnabled_) behSetEnabled_.fn(f, on, behSetEnabled_.mi);
            sp.lpOn = on;
        }
    }

    // -------------------------------------------------------- walls dance ----
    // Box scan: the corrals whose music box plays a song and that the camera is within hearing range
    // of (the speaker's range, Amplify included). Each follows its box's channel: the global queue, or
    // (round 12: "the walls dont dance to the corral's specific radio") the corral's own radio.
    // Audible emitters inside a corral (speaker stacks, Boomboxes, Slime Radios, a DJ Booth's
    // speakers) boost its walls (round 12 idea: "speaker buildables/toy in a plot amplify the walls").
    void SyncDancer() {
        if (!cfg_.danceWalls || !dancer_.Ready() || !AnySong()) return;
        Vec3 cam{};
        const bool haveCam = CameraPos(cam);
        std::vector<dj::WallDancer::Target> targets;
        int boosted = 0;
        for (const Speaker& sp : speakers_) {
            if (sp.emitter || !sp.live || !sp.ch || !sp.ch->cur || sp.clipGen != sp.ch->cur->clipGen) continue;
            Il2CppObject* box = Target(sp.box);
            if (!box) continue;
            auto it = std::find_if(boxUps_.begin(), boxUps_.end(), [&](const auto& b) { return b.first == box; });
            if (it == boxUps_.end() || !unity::IsAlive(it->second)) continue;
            if (haveCam && sp.posOk) {
                const float dx = sp.pos.x - cam.x, dy = sp.pos.y - cam.y, dz = sp.pos.z - cam.z;
                if (dx * dx + dy * dy + dz * dz > sp.maxD * sp.maxD) continue;
            }
            int extra = 0;
            if (sp.corral >= 0)
                for (const Speaker& e : speakers_)
                    if (e.emitter && e.corral == sp.corral && e.ch == sp.ch && Audible(e)) ++extra;
            boosted += extra ? 1 : 0;
            targets.push_back({it->second, reinterpret_cast<uintptr_t>(sp.ch),
                               std::min(1.0f + kDanceSpeakerStep * static_cast<float>(extra), kDanceSpeakerMax)});
        }
        dancer_.Sync(targets);
        const std::array<int, 3> state{static_cast<int>(dancer_.Plots()),
                                       static_cast<int>(std::count_if(targets.begin(), targets.end(),
                                                                      [&](const auto& t) { return t.group != reinterpret_cast<uintptr_t>(&global_); })),
                                       boosted};
        if (state != danceState_) {
            danceState_ = state;
            SR2_INFO("slime_dj: walls dance at {} corral(s) ({} to their Corral Radio, {} boosted by speakers inside)", state[0],
                     state[1], state[2]);
        }
    }

    // Every frame: each channel's analysis at its play position (a bit behind it, see kDanceLagSeconds).
    void DanceSample(Channel& ch) {
        ch.danceHas = false;
        if (!ch.cur) return;
        const Current& c = *ch.cur;
        const bool playing = c.started && c.rate && c.stream && !c.userPaused && !c.buffering && !GamePaused() &&
                             !(c.endFrame && c.playPos >= c.endFrame);
        if (!playing) return;
        const uint64_t lag = static_cast<uint64_t>(kDanceLagSeconds * c.rate);
        const uint64_t pos = c.base + c.playPos;
        const uint64_t q = pos > lag ? pos - lag : 0;
        if (q < ch.danceLast || ch.danceGen != c.clipGen) ch.danceLast = q;  // new song, seek
        ch.danceGen = c.clipGen;
        ch.danceHas = c.stream->beats().Query(ch.danceLast, q, ch.dance);
        ch.danceLast = q;
    }

    void TickDancer(float dt) {
        if (!dancer_.Ready()) return;
        if (!cfg_.danceWalls || !AnySong()) {
            if (dancer_.Plots()) {
                dancer_.Clear();
                danceState_ = {0, 0, 0};
                SR2_INFO("slime_dj: walls dance stopped ({}); walls restored", cfg_.danceWalls ? "no song" : "setting off");
            }
            return;
        }
        DanceSample(global_);
        for (auto& r : radios_) DanceSample(*r);
        dancer_.Tick(dt, [&](uintptr_t group) -> const dj::BeatTrack::Sample* {
            if (group == reinterpret_cast<uintptr_t>(&global_)) return global_.danceHas ? &global_.dance : nullptr;
            for (auto& r : radios_)  // a group is only compared, never dereferenced (a dropped radio's id finds nothing)
                if (group == reinterpret_cast<uintptr_t>(r.get())) return r->danceHas ? &r->dance : nullptr;
            return nullptr;
        }, cfg_.danceIntensity);
    }

    // -------------------------------------------------------- lamps pulse ----
    // Box scan: every speaker that plays its channel's song now (music boxes, emitters: DJ Booth,
    // speaker stacks, Slime Radios, Corral Radio corrals); lamps.cpp pulses the lamps near them.
    void SyncLamps() {
        if (!cfg_.danceLamps || !lamps_.Ready() || !trGetPosition_) return;
        std::vector<dj::LampPulser::Source> src;
        for (const Speaker& sp : speakers_) {
            // volume 0 = an emitter that is switched off (a DJ Booth / Boombox not playing the song)
            if (!sp.live || !sp.ch || !sp.ch->cur || sp.clipGen != sp.ch->cur->clipGen || sp.volScale <= 0.01f) continue;
            Il2CppObject* go = Target(sp.go);
            Il2CppObject* tr = go ? unity::TransformOf(go) : nullptr;
            if (!unity::IsAlive(tr)) continue;
            const Vec3 p = trGetPosition_.fn(tr, trGetPosition_.mi);
            if (Finite(p)) src.push_back({p.x, p.y, p.z, sp.ch});
        }
        lamps_.Sync(src, cfg_.lampRange);
    }

    // Every frame: each channel's song position (read like TickDancer: kDanceLagSeconds behind).
    void TickLamps(float dt) {
        if (!lamps_.Ready()) return;
        if (!cfg_.danceLamps || !AnySong()) {
            if (lamps_.Lamps()) lamps_.Clear();
            return;
        }
        const bool gamePaused = GamePaused();
        std::vector<dj::LampPulser::Channel> chs;
        auto add = [&](const Channel& ch) {
            if (!ch.cur) return;
            const Current& c = *ch.cur;
            dj::LampPulser::Channel x;
            x.key = &ch;
            x.track = c.stream ? &c.stream->beats() : nullptr;
            x.gen = c.clipGen;
            x.playing = c.started && c.rate && c.stream && !c.userPaused && !c.buffering && !gamePaused &&
                        !(c.endFrame && c.playPos >= c.endFrame);
            const uint64_t lag = static_cast<uint64_t>(kDanceLagSeconds * c.rate), pos = c.base + c.playPos;
            x.frame = pos > lag ? pos - lag : 0;
            chs.push_back(x);
        };
        add(global_);
        for (const auto& r : radios_) add(*r);
        lamps_.Tick(dt, chs);
    }

    // One line a minute while box scans ran: their cost, and how often the scene was searched.
    void LogPerf(float dt) {
        perf_.acc += std::isfinite(dt) && dt > 0 ? std::min(dt, 1.0f) : 0.0f;
        if (perf_.acc < kPerfLogInterval) return;
        if (perf_.scans > 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double ms = 1000.0 / static_cast<double>(f.QuadPart);
            SR2_INFO("slime_dj: box scans in the last {:.0f} s: {} (avg {:.3f} ms, max {:.3f} ms); scene searches {}{}; "
                     "{} corral(s) known, {} speaker(s)",
                     perf_.acc, perf_.scans, ms * static_cast<double>(perf_.sum) / perf_.scans, ms * static_cast<double>(perf_.max),
                     perf_.searches, perf_.searches ? std::format(" (max {:.2f} ms)", ms * static_cast<double>(perf_.searchMax)) : "",
                     ups_.size(), speakers_.size());
        }
        perf_ = ScanPerf{};
    }

    // ---------------------------------------------------------------- tick ----
    void Tick(float dt) {
        Guarded("tick", [&] { TickImpl(dt); });
    }

    void TickImpl(float dt) {
        if (cfgDirty_.exchange(false)) {
            const bool danced = cfg_.danceWalls, lamped = cfg_.danceLamps;
            cfg_ = LoadCfg();
            if (cfg_.danceWalls && !danced) boxScan_ = 0;  // walls dance turned on: find the corrals now
            if (cfg_.danceLamps && !lamped) boxScan_ = 0;  // lamps turned on: find them now
        }
        const bool viewed = bridge::WantsSnapshot();
        boxScan_ -= dt;
        // A plot with a music box appeared / a box was bought: scan now. With custom upgrades on, also
        // while idle (one probe scan), so a Corral Radio left on in the save starts after a load.
        if (DrainApplied()) {
            boxScan_ = 0;
            if (cu::Active()) probe_ = true;
        }
        // Song started or tab opened after being idle: one scene search as a safety net (on a click, so
        // a one-off hitch); the hook keeps the cache current from then on.
        const bool radiosOn = std::any_of(radios_.begin(), radios_.end(), [](const auto& r) { return r->on; });
        const bool active = global_.cur || radiosOn || viewed || !menuKey_.empty();
        if (active != wasActive_) {
            wasActive_ = active;
            if (active) fullScan_ = true;
        }
        if (AnySong()) FindBuses(dt);  // nothing to look up while idle
        if (const unsigned g = dj::emitters::Generation(); g != emitterGen_) {
            emitterGen_ = g;  // an emitter was (un)registered: pick it up now
            boxScan_ = 0;
        }
        if ((active || probe_) && boxScan_ <= 0) {
            probe_ = false;
            boxScan_ = kBoxScanInterval;
            const LONGLONG scan0 = Qpc();
            std::vector<Il2CppObject*> boxes = FindBoxes();
            const std::vector<dj::emitters::Entry> emitters = dj::emitters::Collect();
            boxes_ = static_cast<int>(boxes.size());
            emitters_ = static_cast<int>(emitters.size());
            BuildCorrals();
            SyncRadios();
            AllocateSlots();
            if (AnySong()) {
                SyncSpeakers(boxes, emitters);
                AssignSpeakers();
                CheckSpeakers();
                UpdateBoost();
                Resync(global_);
                for (auto& r : radios_) Resync(*r);
                SyncDancer();
                SyncLamps();
            }
            perf_.Scan(Qpc() - scan0);
        }
        LogPerf(dt);
        if (!AnySong() && !speakers_.empty()) DestroySpeakers();
        if (!AnySong() && !viewed) boxes_ = -1;
        TickSong(global_);
        for (size_t i = 0; i < radios_.size(); ++i) TickSong(*radios_[i]);
        TickMix(dt);
        TickMuffle(dt);
        TickDancer(dt);
        TickLamps(dt);
        TickMenu(dt);
        const Current* g = global_.cur.get();
        dj::emitters::SetPlaying(g && g->started && !g->userPaused &&
                                     std::any_of(speakers_.begin(), speakers_.end(), [&](const Speaker& sp) { return Audible(sp) && sp.ch == &global_; }),
                                 g ? g->name : std::string());
        publishAcc_ += dt;
        if (viewed && publishAcc_ >= kPublishInterval) {
            publishAcc_ = 0;
            Publish();
        }
    }

    void Shutdown() {
        menu_.Close("Slime DJ off");
        menuKey_.clear();
        dancer_.Clear();
        danceState_ = {0, 0, 0};
        lamps_.Clear();
        StopAll();
        DropRadios();
        DestroySpeakers();
        FreeUpgraders();
        corrals_.clear();
        wasActive_ = false;
        Free(musicBus_);
        Free(masterBus_);
        Free(cueBus_);
        busRetry_ = 0;
    }

    // ------------------------------------------------------------- radio menu ----
    // The Corral Radio button (custom_upgrades) asks for the menu through api.h OpenRadioMenu; the next
    // tick scans (the radio channel exists once its corral was seen) and opens it. The menu edits the
    // radio through the same requests as the Slime DJ tab (HandleRadio).
public:
    bool RequestMenu(Il2CppObject* plot, std::string* why) {
        auto no = [&](const char* w) {
            if (why) *why = w;
            return false;
        };
        if (dead_.load() || !frameCb_) return no("Slime DJ is off (overlay: Ranch > Slime DJ)");
        if (!menu_.Ready()) return no("the playlist menu is unavailable; use the overlay's Slime DJ tab");
        if (menu_.IsOpen() || !menuKey_.empty()) return no("the playlist menu is already open");
        std::string key = cu::PlotKey(plot);
        if (key.empty()) return no("this corral is not known yet; try again in a moment");
        menuKey_ = std::move(key);
        menuPending_ = true;
        menuAge_ = 0;
        menuRefresh_ = 0;
        boxScan_ = 0;  // find the corral's radio in the next tick
        return true;
    }

private:
    std::shared_ptr<const std::vector<dj::MenuSong>> MenuSongs() {
        RefreshSongIndex();
        if (songs_ != menuSongsFrom_ || !menuSongs_) {
            menuSongsFrom_ = songs_;
            if (!songs_) {
                menuSongs_.reset();
                return menuSongs_;
            }
            auto v = std::make_shared<std::vector<dj::MenuSong>>();
            v->reserve(songs_->size());
            for (const auto& x : *songs_) v->push_back({x.id, x.name, x.seconds, x.error.empty()});
            std::stable_sort(v->begin(), v->end(), [](const dj::MenuSong& a, const dj::MenuSong& b) {
                return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
            });
            menuSongs_ = std::move(v);
        }
        return menuSongs_;
    }

    dj::MenuState MenuStateFor(const Channel* r) {
        dj::MenuState m;
        m.title = RadioName(menuKey_);
        if (!r) {
            m.available = false;
            m.status = menuAge_ < 3.0f ? "Tuning in..."
                                       : "This corral's radio can't play right now: it needs its Music Box, and custom upgrades on.";
            return m;
        }
        m.on = r->on;
        m.shuffle = r->shuffle;
        m.playing = r->index;
        for (const std::string& f : r->playlist) {
            const dj::SongInfo* song = FindSongByFile(f);
            m.playlist.push_back({song ? song->name : std::filesystem::path(f).stem().string(), song ? song->seconds : 0.0,
                                  !song || !song->error.empty()});
        }
        if (r->cur) {
            const Current& c = *r->cur;
            m.status = "Now playing: " + c.name;
            if (GamePaused()) m.status += "  (paused while this menu is open)";
            else if (c.buffering || !c.started) m.status += "  (loading)";
        } else if (r->on && !r->slot) {
            m.status = std::format("On - waiting: {} radios play already (the nearest ones get the next free slot).", kMaxRadioStreams);
        } else if (r->on) {
            m.status = songs_ ? "On" : "On - reading the music folder...";
        } else {
            m.status = r->playlist.empty() ? "Off - add songs, then switch the radio on." : "Off - press Radio to play this playlist here.";
        }
        return m;
    }

    void TickMenu(float dt) {
        if (menuKey_.empty()) return;
        menuAge_ += std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0.0f;
        if (menuPending_) {
            menuPending_ = false;
            Channel* r = FindRadio(menuKey_);
            if (!menu_.Open(MenuStateFor(r), MenuSongs())) {
                menuKey_.clear();
                return;
            }
            SR2_INFO("slime_dj: radio menu for '{}' (plot '{}'){}", RadioName(menuKey_), menuKey_,
                     r ? "" : ": its radio is not found yet (scanning)");
        }
        if (!menu_.IsOpen()) {
            menuKey_.clear();
            return;
        }
        for (const dj::MenuAction& a : menu_.Tick(dt)) {
            if (a.op == dj::MenuOp::kClose) {
                menu_.Close("closed");
                menuKey_.clear();
                return;
            }
            MenuAct(a);
        }
        if ((menuRefresh_ -= dt) <= 0.0f) {
            menuRefresh_ = 0.25f;
            menu_.SetSongs(MenuSongs());
            menu_.Update(MenuStateFor(FindRadio(menuKey_)));
        }
    }

    void MenuAct(const dj::MenuAction& a) {
        Channel* r = FindRadio(menuKey_);
        if (!r) return;
        bridge::Action b;
        b.radio = menuKey_;
        b.index = a.index;
        switch (a.op) {
            case dj::MenuOp::kAdd: {
                const dj::SongInfo* s = FindSong(a.song);
                if (!s) return;
                if (r->playlist.size() >= kMaxPlaylist) {
                    menu_.Note(std::format("The playlist is full ({} songs).", kMaxPlaylist), false);
                    return;
                }
                b.op = bridge::Op::RadioAdd;
                b.song = a.song;
                menu_.Note("Added " + s->name, true);
                break;
            }
            case dj::MenuOp::kRemove: b.op = bridge::Op::RadioRemove; break;
            case dj::MenuOp::kUp: b.op = bridge::Op::RadioUp; break;
            case dj::MenuOp::kDown: b.op = bridge::Op::RadioDown; break;
            case dj::MenuOp::kClear:
                b.op = bridge::Op::RadioClear;
                menu_.Note("Playlist emptied; the radio is off.", true);
                break;
            case dj::MenuOp::kShuffle: b.op = bridge::Op::RadioShuffle; break;
            case dj::MenuOp::kPlayAt: b.op = bridge::Op::RadioPlayAt; break;
            case dj::MenuOp::kToggle:
                if (!r->on && r->playlist.empty()) {
                    menu_.Note("Add songs to the playlist first.", false);
                    return;
                }
                b.op = bridge::Op::RadioToggle;
                menu_.Note(r->on ? "Radio switched off." : "Radio switched on.", true);
                break;
            default: return;
        }
        Handle(b);
        menu_.Update(MenuStateFor(FindRadio(menuKey_)));
    }

private:
    // ------------------------------------------------------------- requests ----
    void Handle(const bridge::Action& a) {
        if (cfgDirty_.exchange(false)) cfg_ = LoadCfg();
        Channel& g = global_;
        auto find = [&](uint64_t uid) {
            return std::find_if(queue_.begin(), queue_.end(), [&](const QueueItem& q) { return q.uid == uid; });
        };
        switch (a.op) {
            case bridge::Op::Play:
                g.failures = 0;
                StartSong(g, a.song);
                boxScan_ = 0;
                break;
            case bridge::Op::Enqueue: {
                const dj::SongInfo* s = FindSong(a.song);
                if (!s) break;
                queue_.push_back({++queueUid_, s->id});
                bridge::SetStatus("Queued: " + s->name, false);
                if (!g.cur) {
                    g.failures = 0;
                    NextSong(g);
                    boxScan_ = 0;
                }
                break;
            }
            case bridge::Op::Remove:
                if (auto it = find(a.uid); it != queue_.end()) queue_.erase(it);
                break;
            case bridge::Op::MoveUp:
                if (auto it = find(a.uid); it != queue_.end() && it != queue_.begin()) std::iter_swap(it, std::prev(it));
                break;
            case bridge::Op::MoveDown:
                if (auto it = find(a.uid); it != queue_.end() && std::next(it) != queue_.end()) std::iter_swap(it, std::next(it));
                break;
            case bridge::Op::ClearQueue: queue_.clear(); break;
            case bridge::Op::TogglePause:
                if (g.cur) {
                    g.cur->userPaused = !g.cur->userPaused;
                } else {
                    g.failures = 0;
                    NextSong(g);
                    boxScan_ = 0;
                }
                break;
            case bridge::Op::Skip:
                g.failures = 0;
                NextSong(g);
                boxScan_ = 0;
                break;
            case bridge::Op::Stop:
                StopCurrent(g);
                bridge::SetStatus("Stopped.", false);
                break;
            case bridge::Op::Seek: SeekTo(a.seconds); break;
            case bridge::Op::Rescan:
                library_.Rescan(folder_);
                bridge::SetStatus("Reading the music folder...", false);
                break;
            default: HandleRadio(a); break;
        }
        Publish();
    }

    // Playlist edits keep `index` on the entry that plays, so the radio goes on with the entry after it.
    void HandleRadio(const bridge::Action& a) {
        Channel* r = FindRadio(a.radio);
        if (!r) {
            bridge::SetStatus("That corral radio is not loaded right now.", true);
            return;
        }
        const int n = static_cast<int>(r->playlist.size());
        const bool valid = a.index >= 0 && a.index < n;
        switch (a.op) {
            case bridge::Op::RadioToggle:
                if (!r->on && r->playlist.empty()) {
                    bridge::SetStatus(RadioName(*r) + ": add songs to its playlist first.", true);
                    return;
                }
                r->on = !r->on;
                bridge::SetStatus(std::format("{} {}.", RadioName(*r), r->on ? "switched on" : "switched off"), false);
                SR2_INFO("slime_dj: radio '{}' switched {}", RadioName(*r), r->on ? "on" : "off");
                break;
            case bridge::Op::RadioSkip:
                if (r->cur) {
                    r->failures = 0;
                    NextSong(*r);
                }
                break;
            case bridge::Op::RadioShuffle: r->shuffle = !r->shuffle; break;
            case bridge::Op::RadioAdd: {
                const dj::SongInfo* s = FindSong(a.song);
                if (!s) return;
                if (r->playlist.size() >= kMaxPlaylist) {
                    bridge::SetStatus(std::format("{}: the playlist is full ({} songs).", RadioName(*r), kMaxPlaylist), true);
                    return;
                }
                r->playlist.push_back(s->file);
                bridge::SetStatus(std::format("{}: added {}", RadioName(*r), s->name), false);
                break;
            }
            case bridge::Op::RadioRemove:
                if (!valid) return;
                r->playlist.erase(r->playlist.begin() + a.index);
                if (a.index < r->index || (a.index == r->index && r->index >= 0)) --r->index;
                if (r->playlist.empty()) r->on = false;
                break;
            case bridge::Op::RadioUp:
            case bridge::Op::RadioDown: {
                const int other = a.op == bridge::Op::RadioUp ? a.index - 1 : a.index + 1;
                if (!valid || other < 0 || other >= n) return;
                std::swap(r->playlist[static_cast<size_t>(a.index)], r->playlist[static_cast<size_t>(other)]);
                if (r->index == a.index) r->index = other;
                else if (r->index == other) r->index = a.index;
                break;
            }
            case bridge::Op::RadioClear:
                r->playlist.clear();
                r->index = -1;
                r->on = false;
                break;
            case bridge::Op::RadioPlayAt:
                if (!valid) return;
                r->on = true;
                r->index = a.index;
                if (r->slot) {
                    r->failures = 0;
                    NextSong(*r, true);
                }
                break;
            default: return;
        }
        SaveRadio(*r);
        boxScan_ = 0;  // start / stop / hand slots out now
    }

    // ------------------------------------------------------------- snapshot ----
    void Publish() {
        RefreshSongIndex();
        bridge::Snapshot s;
        s.tick = GetTickCount64();
        if (songs_ && !uiSongs_) {
            auto v = std::make_shared<std::vector<bridge::Song>>();
            v->reserve(songs_->size());
            for (const auto& x : *songs_) v->push_back({x.id, x.name, x.file, x.seconds, x.error});
            uiSongs_ = std::move(v);
        }
        s.songs = uiSongs_;
        s.scanning = library_.Scanning();
        s.folder = dj::Utf8(folder_.wstring());
        for (const QueueItem& q : queue_) {
            const dj::SongInfo* song = FindSong(q.song);
            s.queue.push_back({q.uid, q.song, song ? song->name : std::string("(missing)"), song ? song->seconds : 0.0});
        }
        s.shuffle = cfg_.shuffle;
        for (const Speaker& sp : speakers_) {
            if (sp.ch != &global_) continue;
            ++s.speakers;
            if (sp.emitter) ++s.emitterSpeakers;
        }
        s.boxes = boxes_;
        s.emitters = emitters_;
        for (const Corral& c : corrals_)
            if (!c.radio.empty()) ++s.radioBoxes;
        if (global_.cur) {
            const Current& c = *global_.cur;
            s.hasSong = true;
            s.song = c.song;
            s.name = c.name;
            s.paused = c.userPaused;
            Position(c, s.position, s.duration);
            if (!c.stream || c.stream->state() == dj::Stream::State::Opening || !c.clip) s.waiting = "Loading...";
            else if (s.speakers == 0)
                s.waiting = boxes_ == 0 && emitters_ == 0 ? "No music box nearby: buy the Music Box upgrade for a corral, or go back to "
                                                            "your ranch."
                            : boxes_ > 0 && s.radioBoxes >= boxes_ && emitters_ == 0
                                ? "Every music box nearby is tuned to its Corral Radio (see Corral radios below)."
                                : "Looking for music boxes...";
            else if (!c.started) s.waiting = "Loading...";
            else if (c.buffering) s.waiting = "Buffering...";
            else if (!c.userPaused && GamePaused()) s.waiting = "Game paused";
        }
        s.radiosAvailable = cu::Active() && landPlotClass_ && compInParent_;
        s.maxRadioStreams = static_cast<int>(kMaxRadioStreams);
        for (const auto& rp : radios_) {
            const Channel& r = *rp;
            bridge::Radio br;
            br.key = r.key;
            br.name = RadioName(r);
            br.distance = r.dist < 1e8f ? r.dist : -1.0f;
            br.on = r.on;
            br.shuffle = r.shuffle;
            br.index = r.index;
            for (const std::string& f : r.playlist) {
                const dj::SongInfo* song = FindSongByFile(f);
                bridge::RadioEntry e;
                e.file = f;
                e.name = song ? song->name : std::filesystem::path(f).stem().string();
                e.seconds = song ? song->seconds : 0.0;
                e.missing = !song || !song->error.empty();
                br.playlist.push_back(std::move(e));
            }
            if (r.cur) {
                const Current& c = *r.cur;
                br.hasSong = true;
                br.now = c.name;
                Position(c, br.position, br.duration);
                if (!c.stream || c.stream->state() == dj::Stream::State::Opening || !c.clip || !c.started) br.waiting = "Loading...";
                else if (!HasSpeaker(r)) br.waiting = "No speaker in this corral right now.";
                else if (c.buffering) br.waiting = "Buffering...";
                else if (GamePaused()) br.waiting = "Game paused";
            } else if (r.on && !r.slot) {
                br.waiting = std::format("Waiting: {} radios play already (the nearest ones get the next free slot).", kMaxRadioStreams);
            } else if (r.on && !songs_) {
                br.waiting = "Reading the music folder...";
            }
            s.radios.push_back(std::move(br));
        }
        std::sort(s.radios.begin(), s.radios.end(), [](const bridge::Radio& x, const bridge::Radio& y) { return x.name < y.name; });
        bridge::Publish(std::move(s));
    }

    static void Position(const Current& c, double& position, double& duration) {
        const uint64_t total = c.stream ? c.stream->totalFrames() : 0;
        duration = c.rate && total ? static_cast<double>(total) / c.rate : c.hintSeconds;
        const uint64_t pos = c.endFrame ? std::min(c.playPos, c.endFrame) : c.playPos;
        position = c.rate ? static_cast<double>(c.base + pos) / c.rate : 0.0;
        if (duration > 0) position = std::min(position, duration);
    }

    // ---------------------------------------------------------------- state ----
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using ArrayNewFn = Il2CppObject* (*)(Il2CppClass*, uintptr_t);
    ObjectNewFn objectNew_ = nullptr;
    ArrayNewFn arrayNew_ = nullptr;

    Il2CppClass* upgraderClass_ = nullptr;
    Il2CppClass* sectrSourceClass_ = nullptr;
    Il2CppClass* cueClass_ = nullptr;
    Il2CppClass* busClass_ = nullptr;
    Il2CppClass* sectrSystemClass_ = nullptr;
    Il2CppClass* gameObjectClass_ = nullptr;
    Il2CppClass* audioSourceClass_ = nullptr;
    Il2CppClass* audioClipClass_ = nullptr;
    Il2CppClass* singleClass_ = nullptr;
    size_t offMusicBox_ = 0, offSourceCue_ = 0, offCueMin_ = 0, offCueMax_ = 0, offCueFalloff_ = 0, offCueBus_ = 0;
    size_t offBusChildren_ = 0, offMasterBus_ = 0;

    const MethodInfo* findByType_ = nullptr;
    const MethodInfo* destroy_ = nullptr;
    const MethodInfo* compsInChildren_ = nullptr;
    const MethodInfo* setParent_ = nullptr;
    const MethodInfo* goCtor_ = nullptr;
    const MethodInfo* addComponent_ = nullptr;
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetPosition_;  // optional (Amplify, Muffle, radios)
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetRight_, trGetForward_;  // optional (Muffle: corral axes)
    // optional: Muffle (AudioLowPassFilter.cutoffFrequency, Behaviour.enabled), corral walls / net / plot
    Il2CppClass* lowPassClass_ = nullptr;
    Il2CppClass* wallUpClass_ = nullptr;
    Il2CppClass* airNetUpClass_ = nullptr;
    Il2CppClass* landPlotClass_ = nullptr;
    size_t offUpgradeWalls_ = 0, offAirNets_ = 0;
    const MethodInfo* compInParent_ = nullptr;
    M<void (*)(Il2CppObject*, float, const MethodInfo*)> lpSetCutoff_;
    M<void (*)(Il2CppObject*, bool, const MethodInfo*)> behSetEnabled_;

    M<Il2CppObject* (*)(Il2CppString*, int32_t, int32_t, int32_t, bool, const MethodInfo*)> clipCreate_;
    M<bool (*)(Il2CppObject*, Il2CppArray*, int32_t, const MethodInfo*)> clipSetData_;
    M<void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> srcSetClip_;
    M<void (*)(Il2CppObject*, bool, const MethodInfo*)> srcSetLoop_, srcSetPlayOnAwake_, srcSetMute_;
    M<bool (*)(Il2CppObject*, const MethodInfo*)> srcGetMute_;
    M<void (*)(Il2CppObject*, float, const MethodInfo*)> srcSetSpatialBlend_, srcSetMinDistance_, srcSetMaxDistance_,
        srcSetDoppler_, srcSetVolume_;
    M<void (*)(Il2CppObject*, int32_t, const MethodInfo*)> srcSetRolloff_, srcSetPriority_, srcSetTimeSamples_;
    M<int32_t (*)(Il2CppObject*, const MethodInfo*)> srcGetTimeSamples_;
    M<bool (*)(Il2CppObject*, const MethodInfo*)> srcIsPlaying_;
    M<void (*)(Il2CppObject*, int32_t, const MethodInfo*)> objSetHideFlags_;  // optional
    M<void (*)(Il2CppObject*, const MethodInfo*)> srcPlay_, srcPause_, srcUnPause_, srcStop_;
    // SECTR_AudioSource.Volume, SECTR_AudioBus getters
    M<float (*)(Il2CppObject*, const MethodInfo*)> sectrGetVolume_, busEffVolume_;
    M<void (*)(Il2CppObject*, float, const MethodInfo*)> sectrSetVolume_;
    M<bool (*)(Il2CppObject*, const MethodInfo*)> busMuted_, busPaused_;

    std::filesystem::path folder_;
    dj::Library library_;
    dj::SongList songs_;
    unsigned songsGen_ = ~0u;
    std::unordered_map<uint64_t, size_t> songIndex_;
    std::unordered_map<std::string, size_t> fileIndex_;
    std::shared_ptr<const std::vector<bridge::Song>> uiSongs_;

    Channel global_;                               // the global queue
    std::vector<std::unique_ptr<Channel>> radios_; // Corral Radios loaded now (pointers stable for Speaker::ch)
    std::vector<Corral> corrals_;                  // last box scan
    std::deque<QueueItem> queue_;
    uint64_t queueUid_ = 0;
    unsigned clipGen_ = 0;  // across all channels: a clip generation names one channel's clip
    std::vector<Speaker> speakers_;
    std::vector<std::pair<Il2CppObject*, Il2CppObject*>> boxUps_;  // last box scan: MusicBox GameObject, its MusicBoxUpgrader
    std::vector<Il2CppGCHandle> ups_;  // known MusicBoxUpgraders (scene search + Apply hook)
    bool hooked_ = false, fullScan_ = true, dropApplied_ = false, wasActive_ = false, probe_ = false;
    ScanPerf perf_;
    dj::WallDancer dancer_;
    dj::LampPulser lamps_;          // "Lamps pulse to the music" (lamps.h)
    dj::RadioMenu menu_;            // Corral Radio playlist menu (radio_menu.h)
    std::string menuKey_;           // plot key of the radio the menu is for ("" = no menu)
    bool menuPending_ = false;      // requested, opens in the next tick
    float menuAge_ = 0, menuRefresh_ = 0;
    dj::SongList menuSongsFrom_;
    std::shared_ptr<const std::vector<dj::MenuSong>> menuSongs_;
    std::array<int, 3> danceState_{0, 0, 0};  // walls dance: corrals, of them to a radio, boosted (logged on a change)
    int boxes_ = -1, emitters_ = 0, resyncs_ = 0, restartLogs_ = 0;
    std::array<int, 5> lastCounts_{-1, -1, -1, -1, -1};
    std::array<int, 2> lastCorralCounts_{-1, -1};
    int channelLogs_ = 0;
    bool radioSaveWarned_ = false, lowPassFailLogged_ = false, sumCapLogged_ = false;
    unsigned emitterGen_ = 0;
    Il2CppGCHandle musicBus_ = nullptr, masterBus_ = nullptr, cueBus_ = nullptr;
    float boxScan_ = 0, busRetry_ = 0, publishAcc_ = 0;
    std::mt19937_64 rng_{GetTickCount64()};

    Cfg cfg_;
    std::atomic<bool> cfgDirty_{true};
    std::atomic<bool> dead_{false};
    int frameCb_ = 0, onChange_ = 0, exceptions_ = 0;
    bool scannedOnce_ = false, busTreeLogged_ = false, speakerLogged_ = false, speakerFailLogged_ = false;
};

// Never deleted (process exit may run after the IL2CPP runtime is gone).
class SlimeDjModule final : public core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(core::ModuleContext& ctx) override { return impl_->Setup(ctx); }
    void OnEnable() override { impl_->OnEnable(); }
    void OnDisable() override { impl_->OnDisable(); }

private:
    SlimeDj* impl_ = new SlimeDj();
};

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSlimeDj() { return std::make_unique<SlimeDjModule>(); }

namespace slime_dj {

bool OpenRadioMenu(Il2CppObject* landPlot, std::string* why) {
    if (!sr2::core::main_thread::IsMainThread() || !landPlot) {
        if (why) *why = "not on the main thread";
        return false;
    }
    SlimeDj* self = g_self;
    if (!self) {
        if (why) *why = "Slime DJ is off (overlay: Ranch > Slime DJ)";
        return false;
    }
    return self->RequestMenu(landPlot, why);
}

}  // namespace slime_dj

}  // namespace sr2::modules
