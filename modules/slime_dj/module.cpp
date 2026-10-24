// slime_dj: custom songs from the corral Music Box ("slime speakers").
//
// Songs: .ogg / .mp3 / .wav files in <game>\sr2mods_music (created on the first start; subfolders
// are read too). library.cpp reads the folder on a background thread; stream.cpp decodes the
// playing song on another one (compiled-in stb_vorbis / dr_mp3 / dr_wav, codecs.c). The main
// thread only copies decoded PCM into the game's audio.
//
// Playback (main thread, one frame callback, no hooks):
//   * Music boxes: Object.FindObjectsByType(MusicBoxUpgrader, exclude inactive) every 2 s while a
//     song is loaded or the tab is open; a box counts while MusicBoxUpgrader.MusicBox (the
//     GameObject the upgrade switches on) is active. Corrals without the upgrade are skipped.
//   * Every owned box nearby gets a speaker: a GameObject "sr2dj_Speaker" with a Unity
//     AudioSource, parented to the box's SECTR audio source (or the box itself), fully 3D with
//     the vanilla cue's MinDistance / MaxDistance / falloff. All speakers play the same clip in
//     sync (started together; a box that appears later joins at the current position), so
//     walking from one corral to the next never restarts the song. At most kMaxSpeakers.
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
//
// Fault policy (like wall_markers): every game call runs inside one guarded tick or guarded
// request; a native fault marks the module faulted and stops it for the session. GC handles are
// freed only in the tick / requests (never in a hook; there are none).

#include <windows.h>

#include <algorithm>
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
#include "modules/slime_dj/emitters.h"
#include "modules/slime_dj/library.h"
#include "modules/slime_dj/stream.h"
#include "ui/slime_dj_bridge.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace bridge = sr2::ui::slime_dj;
namespace dj = sr2::modules::slime_dj;
using il2cpp::api;
using il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kAudio = "UnityEngine.AudioModule";
constexpr const wchar_t* kFolderName = L"sr2mods_music";
constexpr const char* kSpeakerName = "sr2dj_Speaker";
constexpr size_t kMaxSpeakers = 12;      // music boxes
constexpr size_t kMaxEmitters = 24;      // registered emitters (api.h)
constexpr uint32_t kRingChunks = 16;     // 8 s ring at 0.5 s chunks
constexpr uint32_t kStartChunks = 4;     // written before the first Play (2 s)
constexpr int kWritesPerTick = 3;        // chunks copied per frame once playing
constexpr float kBoxScanInterval = 2.0f;
constexpr float kBusRetryInterval = 5.0f;
constexpr float kPublishInterval = 0.2f;
constexpr int kMaxExceptions = 20;
constexpr int kMaxFailuresInARow = 3;
// 3D: linear rolloff, full volume up to kFullFraction of the hearing range, silent at the range.
constexpr float kFullFraction = 0.2f;
// Amplify: each other speaker within hearing range adds kAmpStep (volume and range), up to kAmpMax.
constexpr float kAmpStep = 0.25f, kAmpMax = 2.0f;
// Speakers more than this far from the clock speaker are moved back to it (two scans in a row).
constexpr double kDriftSeconds = 0.03;

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
};

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
    // emitter (api.h): owner key as registered, scales; music boxes: nullptr, 1, 1
    Il2CppObject* emitter = nullptr;
    float rangeScale = 1.0f, volScale = 1.0f;
    float boost = 1.0f;    // amplify (other speakers in range)
    int drift = 0;         // scans in a row it was off the clock speaker
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
};

struct QueueItem {
    uint64_t uid = 0, song = 0;
};

class SlimeDj;
SlimeDj* g_self = nullptr;

class SlimeDj {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string why;
        if (!Resolve(why)) return ctx.Fail(why);
        RegisterSettings();
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        folder_ = paths::SelfDir() / kFolderName;
        std::error_code ec;
        if (!std::filesystem::is_directory(folder_, ec)) {
            if (std::filesystem::create_directories(folder_, ec))
                SR2_INFO("slime_dj: created the music folder {}", dj::Utf8(folder_.wstring()));
            else
                SR2_WARN("slime_dj: cannot create the music folder {}: {}", dj::Utf8(folder_.wstring()), ec.message());
        }
        SR2_INFO("slime_dj: resolved (AudioClip/AudioSource, MusicBoxUpgrader; SECTR source {}, cue {}, bus {}, system {})",
                 sectrSourceClass_ ? "yes" : "no", cueClass_ ? "yes" : "no", busClass_ ? "yes" : "no",
                 sectrSystemClass_ ? "yes" : "no");
        return true;
    }

    void OnEnable() {
        if (dead_.load()) {
            SR2_WARN("slime_dj: stays off for this session after a native fault");
            return;
        }
        g_self = this;
        cfgDirty_.store(true);
        if (!scannedOnce_) {
            scannedOnce_ = true;
            library_.Rescan(folder_);
        }
        bridge::SetHandler(&OnRequest);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
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
        as(srcSetTimeSamples_, "set_timeSamples", 1);
        as(srcPause_, "Pause", 0);
        as(srcUnPause_, "UnPause", 0);
        as(srcStop_, "Stop", 0);
        bind(srcPlay_, byParams(audioSourceClass_, "Play", {}), "AudioSource.Play()");
        return why.empty();
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
            "Each other music box or speaker within hearing range of a box makes it 25 % louder and carry 25 % "
            "farther, up to double. All boxes always play the song in sync, so boxes close together also add up "
            "on their own.");
        settings::SetDisplay(kModule, "volume", "%", {}, 100);
        settings::SetDisplay(kModule, "range", " m");
    }

    static Cfg LoadCfg() {
        Cfg c;
        c.volume = std::clamp(settings::GetFloat(kModule, "volume"), 0.0f, 1.0f);
        c.range = std::clamp(settings::GetFloat(kModule, "range"), 5.0f, 500.0f);
        if (!std::isfinite(c.range)) c.range = 60.0f;
        c.amplify = settings::GetBool(kModule, "amplify");
        c.shuffle = settings::GetBool(kModule, "shuffle");
        c.muteVanilla = settings::GetBool(kModule, "mute_vanilla");
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
        dead_.store(true);
        bridge::SetHandler(nullptr);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        if (cur_ && cur_->stream) cur_->stream->Stop();
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
        if (songs_)
            for (size_t i = 0; i < songs_->size(); ++i) songIndex_.emplace((*songs_)[i].id, i);
        uiSongs_.reset();
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
    // Owned music boxes nearby (active MusicBox GameObjects), in scene order.
    std::vector<Il2CppObject*> FindBoxes() {
        std::vector<Il2CppObject*> out;
        Il2CppObject* type = unity::TypeOf(upgraderClass_);
        if (!type) return out;
        int32_t inactive = 0 /* FindObjectsInactive.Exclude */, sort = 0 /* FindObjectsSortMode.None */;
        bool ok = false;
        Il2CppObject* arr = Call(findByType_, nullptr, {type, &inactive, &sort}, "Object.FindObjectsByType", &ok);
        if (!ok || !arr) return out;
        for (size_t i = 0, n = std::min<size_t>(unity::ArrayLength(arr), 256); i < n; ++i) {
            auto* up = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!unity::IsAlive(up) || !IsA(up, upgraderClass_)) continue;
            Il2CppObject* box = unity::Field<Il2CppObject*>(up, offMusicBox_);
            if (!unity::IsAlive(box) || !IsA(box, gameObjectClass_) || !unity::ActiveInHierarchy(box)) continue;
            if (std::find(out.begin(), out.end(), box) == out.end()) out.push_back(box);
        }
        return out;
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
        sp.box = unity::NewGCHandle(box);
        sp.go = unity::NewGCHandle(go);
        sp.src = unity::NewGCHandle(src);
        ApplyRange(sp, src);
        if (e) {
            SR2_INFO("slime_dj: speaker on emitter '{}' (kind {}, range x{:.2f}, volume x{:.2f})", unity::GetName(box),
                     static_cast<int>(e->kind), e->range, e->volume);
        } else if (!speakerLogged_) {
            speakerLogged_ = true;
            SR2_INFO("slime_dj: speaker on music box '{}' at '{}': {} vanilla source(s), {}; hearing range {:.0f} m",
                     unity::GetName(box), unity::GetName(anchor), sp.vanilla.size(),
                     found.empty() ? "no SECTR cue" : found, sp.maxD);
        }
        return sp.box && sp.go && sp.src;
    }

    // Hearing range x the emitter's scale x Amplify's boost; set when it changed.
    void ApplyRange(Speaker& sp, Il2CppObject* src) {
        const float maxD = std::clamp(cfg_.range * sp.rangeScale * sp.boost, 2.0f, 1000.0f);
        if (std::fabs(maxD - sp.maxD) < 0.05f) return;
        srcSetMinDistance_.fn(src, std::max(0.5f, maxD * kFullFraction), srcSetMinDistance_.mi);
        srcSetMaxDistance_.fn(src, maxD, srcSetMaxDistance_.mi);
        sp.maxD = maxD;
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

    // Amplify: every speaker counts the others within its hearing range (positions every box scan).
    void UpdateBoost() {
        const bool on = cfg_.amplify && trGetPosition_ && speakers_.size() > 1;
        std::vector<Vec3> pos(speakers_.size());
        std::vector<bool> known(speakers_.size(), false);
        if (on)
            for (size_t i = 0; i < speakers_.size(); ++i)
                if (Il2CppObject* go = Target(speakers_[i].go); go && unity::IsAlive(unity::TransformOf(go))) {
                    pos[i] = trGetPosition_.fn(unity::TransformOf(go), trGetPosition_.mi);
                    known[i] = std::isfinite(pos[i].x) && std::isfinite(pos[i].y) && std::isfinite(pos[i].z);
                }
        for (size_t i = 0; i < speakers_.size(); ++i) {
            int n = 0;
            const float r = cfg_.range * speakers_[i].rangeScale;
            for (size_t j = 0; on && known[i] && j < speakers_.size(); ++j) {
                if (j == i || !known[j]) continue;
                const float dx = pos[i].x - pos[j].x, dy = pos[i].y - pos[j].y, dz = pos[i].z - pos[j].z;
                if (dx * dx + dy * dy + dz * dz <= r * r) ++n;
            }
            speakers_[i].boost = std::min(1.0f + kAmpStep * static_cast<float>(n), kAmpMax);
        }
    }

    // Keeps every speaker on the clock speaker's sample position. Play() starts at the next audio
    // update, so a speaker that joined later can trail by a mix buffer; one that is off by more than
    // kDriftSeconds on two box scans in a row is moved back (all readings come from the same audio
    // update, so they compare directly).
    void Resync(Il2CppObject* master) {
        if (!cur_ || !master || !cur_->started || cur_->ring == 0) return;
        const Current& c = *cur_;
        const int32_t ref = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
        if (ref < 0 || static_cast<uint32_t>(ref) >= c.ring) return;
        const int64_t limit = static_cast<int64_t>(kDriftSeconds * c.rate);
        for (Speaker& sp : speakers_) {
            Il2CppObject* src = Target(sp.src);
            if (!src || src == master || sp.clipGen != c.clipGen || sp.paused) {
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

    // --------------------------------------------------------------- songs ----
    void StopCurrent() {
        if (!cur_) return;
        if (cur_->stream) cur_->stream->Stop();
        // The vanilla tune stays silenced while the next song loads; TickMix / DestroySpeakers
        // bring it back when nothing plays.
        for (Speaker& sp : speakers_) {
            if (Il2CppObject* src = Target(sp.src)) {
                srcStop_.fn(src, srcStop_.mi);
                srcSetClip_.fn(src, nullptr, srcSetClip_.mi);
            }
            sp.clipGen = 0;
            sp.paused = false;
        }
        Destroy(Target(cur_->clip));
        Free(cur_->clip);
        Free(cur_->buf);
        cur_.reset();
    }

    bool StartSong(uint64_t id) {
        const dj::SongInfo* s = FindSong(id);
        if (!s) {
            bridge::SetStatus("That song is no longer in the music folder. Rescan?", true);
            return false;
        }
        if (!s->error.empty()) {
            bridge::SetStatus(std::format("Can't play {}: {}", s->name, s->error), true);
            return false;
        }
        StopCurrent();
        cur_ = std::make_unique<Current>();
        cur_->song = s->id;
        cur_->name = s->name;
        cur_->hintSeconds = s->seconds;
        cur_->stream = dj::Stream::Start(s->path);
        lastSong_ = s->id;
        SR2_INFO("slime_dj: playing '{}' ({})", s->name, s->file);
        bridge::SetStatus("Now playing: " + s->name, false);
        return true;
    }

    // Next song: the queue's first, else a random one (shuffle), else stop.
    void NextSong() {
        StopCurrent();
        while (!queue_.empty()) {
            const uint64_t id = queue_.front().song;
            queue_.pop_front();
            if (StartSong(id)) return;
        }
        if (cfg_.shuffle) {
            RefreshSongIndex();
            std::vector<uint64_t> pool;
            if (songs_)
                for (const auto& s : *songs_)
                    if (s.error.empty()) pool.push_back(s.id);
            if (pool.size() > 1) std::erase(pool, lastSong_);
            if (!pool.empty()) {
                std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
                if (StartSong(pool[pick(rng_)])) return;
            } else if (!songs_ || songs_->empty()) {
                bridge::SetStatus("No songs in the music folder yet: add .ogg, .mp3 or .wav files, then Rescan.", true);
                return;
            }
        }
        bridge::SetStatus("Queue finished.", false);
    }

    void SongFailed(const std::string& why) {
        SR2_WARN("slime_dj: '{}' failed: {}", cur_ ? cur_->name : std::string("?"), why);
        bridge::SetStatus(std::format("Can't play {}: {}", cur_ ? cur_->name : std::string("the song"), why), true);
        if (++failuresInARow_ >= kMaxFailuresInARow) {
            failuresInARow_ = 0;
            StopCurrent();
            return;
        }
        NextSong();
    }

    bool CreateClip() {
        Current& c = *cur_;
        c.channels = c.stream->channels();
        c.rate = c.stream->rate();
        c.chunk = c.stream->chunkFrames();
        c.ring = c.chunk * kRingChunks;
        Il2CppObject* clip = clipCreate_.fn(unity::NewString(std::format("sr2dj_{}", c.song)), static_cast<int32_t>(c.ring),
                                            static_cast<int32_t>(c.channels), static_cast<int32_t>(c.rate), false,
                                            clipCreate_.mi);
        if (!unity::IsAlive(clip) || !IsA(clip, audioClipClass_)) return false;
        c.clip = unity::NewGCHandle(clip);
        Il2CppObject* buf = arrayNew_(singleClass_, static_cast<uintptr_t>(c.chunk) * c.channels);
        if (!buf || unity::ArrayLength(buf) != static_cast<size_t>(c.chunk) * c.channels) return false;
        c.buf = unity::NewGCHandle(buf);
        c.clipGen = ++clipGen_;
        return c.clip && c.buf;
    }

    // Copies chunks into the ring ahead of the play head (never into the chunk being played).
    // False when the song was dropped (cur_ replaced or gone).
    bool Fill(int maxWrites) {
        Current& c = *cur_;
        Il2CppObject* clip = Target(c.clip);
        auto* buf = unity::GCHandleTarget(c.buf);
        if (!clip || !buf) {
            SongFailed("the AudioClip was destroyed");
            return false;
        }
        float* dst = unity::ArrayData<float>(buf);
        const size_t n = static_cast<size_t>(c.chunk) * c.channels;
        const uint64_t playChunkStart = c.playPos / c.chunk * c.chunk;
        for (int i = 0; i < maxWrites && c.writeHead + c.chunk <= playChunkStart + c.ring; ++i) {
            uint32_t frames = 0;
            if (c.stream->Pop(c.tmp, frames) && c.tmp.size() == n) {
                std::memcpy(dst, c.tmp.data(), n * sizeof(float));
                c.realWritten += frames;
            } else if (c.stream->Done()) {
                if (!c.endFrame) c.endFrame = std::max<uint64_t>(c.realWritten, 1);
                std::memset(dst, 0, n * sizeof(float));  // silence after the end
            } else {
                break;  // the decoder is behind
            }
            const int32_t offset = static_cast<int32_t>(c.writeHead % c.ring);
            if (!clipSetData_.fn(clip, static_cast<Il2CppArray*>(static_cast<void*>(buf)), offset, clipSetData_.mi)) {
                SongFailed("AudioClip.SetData refused the data");
                return false;
            }
            c.writeHead += c.chunk;
        }
        return true;
    }

    Il2CppObject* Master() {
        for (Speaker& sp : speakers_)
            if (cur_ && sp.clipGen == cur_->clipGen)
                if (Il2CppObject* src = Target(sp.src)) return src;
        return nullptr;
    }

    void TickSong() {
        if (!cur_) return;
        Current& c = *cur_;
        if (c.stream->state() == dj::Stream::State::Opening) return;
        if (c.stream->state() == dj::Stream::State::Failed) {
            SongFailed(c.stream->error());
            return;
        }
        if (!c.clip && !CreateClip()) {
            SongFailed("AudioClip.Create failed");
            return;
        }
        // Play head.
        Il2CppObject* master = Master();
        if (c.started && master) {
            const int32_t ts = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
            if (master != c.lastMaster) {
                c.lastMaster = master;  // another speaker took over: continue from its position
            } else if (ts >= 0 && static_cast<uint32_t>(ts) < c.ring) {
                const uint32_t delta = (static_cast<uint32_t>(ts) + c.ring - static_cast<uint32_t>(c.lastTs)) % c.ring;
                if (delta < c.ring - c.chunk) c.playPos += delta;  // a tiny step back = another speaker's jitter
            }
            c.lastTs = ts;
        }
        if (cur_->endFrame && c.playPos >= c.endFrame) {
            failuresInARow_ = 0;
            NextSong();
            return;
        }
        if (!Fill(c.started ? kWritesPerTick : static_cast<int>(kRingChunks))) return;
        // Buffering: the decoder fell behind the play head.
        const uint64_t ahead = c.writeHead > c.playPos ? c.writeHead - c.playPos : 0;
        if (!c.stream->Done()) {
            if (c.started && ahead < c.chunk / 4) c.buffering = true;
            else if (c.buffering && (ahead >= 2ull * c.chunk || c.stream->Queued() >= 2)) c.buffering = false;
        } else {
            c.buffering = false;
        }
        // First start once enough is written.
        const bool ready = c.writeHead >= static_cast<uint64_t>(kStartChunks) * c.chunk || c.stream->Done();
        Il2CppObject* clip = Target(c.clip);
        const bool wantPause = c.userPaused || c.buffering || GamePaused() || speakers_.empty();
        // Where a (re)joining speaker starts: the playing speakers' position, else where the song
        // stopped when the last box went away (the ring still holds what follows).
        int32_t joinAt = static_cast<int32_t>(c.playPos % c.ring);
        if (master) joinAt = srcGetTimeSamples_.fn(master, srcGetTimeSamples_.mi);
        for (Speaker& sp : speakers_) {
            Il2CppObject* src = Target(sp.src);
            if (!src || !clip) continue;
            if (sp.clipGen != c.clipGen) {
                if (!ready) continue;
                srcSetClip_.fn(src, clip, srcSetClip_.mi);
                srcSetLoop_.fn(src, true, srcSetLoop_.mi);
                srcSetTimeSamples_.fn(src, joinAt, srcSetTimeSamples_.mi);
                srcPlay_.fn(src, srcPlay_.mi);
                sp.clipGen = c.clipGen;
                sp.paused = false;
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

    void TickMix() {
        const float vol = std::clamp(cfg_.volume * MusicVolume(), 0.0f, 2.0f);
        const bool mute = cur_ && !cur_->userPaused && cfg_.muteVanilla;
        for (Speaker& sp : speakers_) {
            // AudioSource.volume tops out at 1: Amplify's extra volume only helps below that (its
            // range boost always does).
            const float v = std::clamp(vol * sp.volScale * sp.boost, 0.0f, 1.0f);
            Il2CppObject* src = Target(sp.src);
            if (src && std::fabs(sp.volume - v) > 0.002f) {
                srcSetVolume_.fn(src, v, srcSetVolume_.mi);
                sp.volume = v;
            }
            if (src) ApplyRange(sp, src);
            if (mute) Mute(sp);
            else Unmute(sp);
        }
    }

    // ---------------------------------------------------------------- tick ----
    void Tick(float dt) {
        Guarded("tick", [&] { TickImpl(dt); });
    }

    void TickImpl(float dt) {
        if (cfgDirty_.exchange(false)) cfg_ = LoadCfg();
        const bool viewed = bridge::WantsSnapshot();
        if (cur_) FindBuses(dt);  // nothing to look up while idle
        boxScan_ -= dt;
        if (const unsigned g = dj::emitters::Generation(); g != emitterGen_) {
            emitterGen_ = g;  // an emitter was (un)registered: pick it up now
            boxScan_ = 0;
        }
        if ((cur_ || viewed) && boxScan_ <= 0) {
            boxScan_ = kBoxScanInterval;
            std::vector<Il2CppObject*> boxes = FindBoxes();
            const std::vector<dj::emitters::Entry> emitters = dj::emitters::Collect();
            boxes_ = static_cast<int>(boxes.size());
            emitters_ = static_cast<int>(emitters.size());
            if (cur_) {
                SyncSpeakers(boxes, emitters);
                UpdateBoost();
                Resync(Master());
            }
        }
        if (!cur_ && !speakers_.empty()) DestroySpeakers();
        if (!cur_ && !viewed) boxes_ = -1;
        TickSong();
        TickMix();
        dj::emitters::SetPlaying(cur_ && cur_->started && !cur_->userPaused && !speakers_.empty(),
                                 cur_ ? cur_->name : std::string());
        publishAcc_ += dt;
        if (viewed && publishAcc_ >= kPublishInterval) {
            publishAcc_ = 0;
            Publish();
        }
    }

    void Shutdown() {
        StopCurrent();
        DestroySpeakers();
        Free(musicBus_);
        Free(masterBus_);
        Free(cueBus_);
        busRetry_ = 0;
    }

    // ------------------------------------------------------------- requests ----
    void Handle(const bridge::Action& a) {
        if (cfgDirty_.exchange(false)) cfg_ = LoadCfg();
        auto find = [&](uint64_t uid) {
            return std::find_if(queue_.begin(), queue_.end(), [&](const QueueItem& q) { return q.uid == uid; });
        };
        switch (a.op) {
            case bridge::Op::Play:
                failuresInARow_ = 0;
                StartSong(a.song);
                boxScan_ = 0;
                break;
            case bridge::Op::Enqueue: {
                const dj::SongInfo* s = FindSong(a.song);
                if (!s) break;
                queue_.push_back({++queueUid_, s->id});
                bridge::SetStatus("Queued: " + s->name, false);
                if (!cur_) {
                    failuresInARow_ = 0;
                    NextSong();
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
                if (cur_) {
                    cur_->userPaused = !cur_->userPaused;
                } else {
                    failuresInARow_ = 0;
                    NextSong();
                    boxScan_ = 0;
                }
                break;
            case bridge::Op::Skip:
                failuresInARow_ = 0;
                NextSong();
                boxScan_ = 0;
                break;
            case bridge::Op::Stop:
                StopCurrent();
                bridge::SetStatus("Stopped.", false);
                break;
            case bridge::Op::Rescan:
                library_.Rescan(folder_);
                bridge::SetStatus("Reading the music folder...", false);
                break;
        }
        Publish();
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
        s.speakers = static_cast<int>(speakers_.size());
        s.emitterSpeakers = static_cast<int>(std::count_if(speakers_.begin(), speakers_.end(), [](const Speaker& sp) { return sp.emitter; }));
        s.boxes = boxes_;
        s.emitters = emitters_;
        if (cur_) {
            const Current& c = *cur_;
            s.hasSong = true;
            s.song = c.song;
            s.name = c.name;
            s.paused = c.userPaused;
            const uint64_t total = c.stream ? c.stream->totalFrames() : 0;
            s.duration = c.rate && total ? static_cast<double>(total) / c.rate : c.hintSeconds;
            const uint64_t pos = c.endFrame ? std::min(c.playPos, c.endFrame) : c.playPos;
            s.position = c.rate ? static_cast<double>(pos) / c.rate : 0.0;
            if (s.duration > 0) s.position = std::min(s.position, s.duration);
            if (!c.stream || c.stream->state() == dj::Stream::State::Opening || !c.clip) s.waiting = "Loading...";
            else if (speakers_.empty())
                s.waiting = boxes_ == 0 && emitters_ == 0 ? "No music box nearby: buy the Music Box upgrade for a corral, or go back to "
                                          "your ranch."
                                        : "Looking for music boxes...";
            else if (!c.started) s.waiting = "Loading...";
            else if (c.buffering) s.waiting = "Buffering...";
            else if (!c.userPaused && GamePaused()) s.waiting = "Game paused";
        }
        bridge::Publish(std::move(s));
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
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetPosition_;  // optional (Amplify)

    M<Il2CppObject* (*)(Il2CppString*, int32_t, int32_t, int32_t, bool, const MethodInfo*)> clipCreate_;
    M<bool (*)(Il2CppObject*, Il2CppArray*, int32_t, const MethodInfo*)> clipSetData_;
    M<void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> srcSetClip_;
    M<void (*)(Il2CppObject*, bool, const MethodInfo*)> srcSetLoop_, srcSetPlayOnAwake_, srcSetMute_;
    M<bool (*)(Il2CppObject*, const MethodInfo*)> srcGetMute_;
    M<void (*)(Il2CppObject*, float, const MethodInfo*)> srcSetSpatialBlend_, srcSetMinDistance_, srcSetMaxDistance_,
        srcSetDoppler_, srcSetVolume_;
    M<void (*)(Il2CppObject*, int32_t, const MethodInfo*)> srcSetRolloff_, srcSetPriority_, srcSetTimeSamples_;
    M<int32_t (*)(Il2CppObject*, const MethodInfo*)> srcGetTimeSamples_;
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
    std::shared_ptr<const std::vector<bridge::Song>> uiSongs_;

    std::unique_ptr<Current> cur_;
    std::deque<QueueItem> queue_;
    uint64_t queueUid_ = 0, lastSong_ = 0;
    unsigned clipGen_ = 0;
    int failuresInARow_ = 0;
    std::vector<Speaker> speakers_;
    int boxes_ = -1, emitters_ = 0, resyncs_ = 0;
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

}  // namespace sr2::modules
