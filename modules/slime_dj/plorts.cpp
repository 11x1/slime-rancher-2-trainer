// slime_dj: "Plorts glow to the music" (see plorts.h, NOTES.md).
//
// Plort prefabs (identifiabletypes bundle, UnityPy, game 1.3.0): the root has the body MeshRenderer, a
// child "Shield" (material plortShield, SR/Actor, Vac (Glass): no colour gradient, never touched) and an
// EnvIntEntity quad. Body shaders: SR/AMP/Slime/Plort (most plorts), "Plort (Glow)" (Phosphor: also HDR
// _GlowTop), "Plort MatCap" (Gold), "Plort (Boom)" (Boom, Fire), "Plort Unstable" (Unstable, Stable),
// "Plort Shadow" (also _PulseGlowColor). None of them has a real emission input (the declared
// _EmissionColor is the HDRP template's GI colour, white on every plort), so the glow is the body
// gradient itself: _TopColor / _MiddleColor / _BottomColor (on every plort shader and on attachments such
// as the crystal spikes), plus _TwinTopColor / _TwinMiddleColor / _TwinBottomColor (Twin), _GlowTop and
// _PulseGlowColor where the shader has them. Each is scaled by the beat factor and lifted a little in its
// own hue (so dark plorts glow too):
//   c' = c x s + (c / max(c)) x kLift x (s - 1),  s = 1 + kLevel e + kBeat p
// with e = w x bass level, p = w x beat pulse, w from the speakers of the plort's channel in range
// (the lamps' kStack rule). Colours keep their hue (no colour cycling).
// The plort's own glow (round 17: "they still have their default glow"): the shaders' property lists
// (shaders bundle, UnityPy) have one glow input each: _PulseGlowIntensity (SR/AMP/Slime/Plort, Unstable,
// Shadow: "Pulse Glow Intensity", 1 on most plorts, 5000 on Shadow with a dark _PulseGlowColor) and the
// Phosphor "Plort (Glow)" animation (_GlowSpeed 0.8, _GlowMin 0, _GlowMax 1, _GlowStrength 1). While a
// plort pulses, _PulseGlowIntensity goes to 0 (faded with the plort's strength easing in), and Phosphor's
// glow stops animating (_GlowSpeed 0) and follows the beat (_GlowMin = _GlowMax = beat amount between
// the material's min and max) instead of its _GlowTop being scaled. Restore puts the renderer's own block
// back, so the game's glow returns.
// A renderer is a glowing part when its material has _TopColor, _MiddleColor and _BottomColor.
// Base colours: the renderer's own block (RecolorSlimeMaterial writes these three through a block on
// slimes; a block without the colour reads back as (0, 0, 0, 0)), else the shared material.
//
// Finding the plorts: GameModel.plortGroup (IdentifiableTypeGroup and its sub-groups) = the plort
// types (found once, retried every 10 s); GameModel.identifiablesByIdent[type] = the ActorModels of
// every loose plort (GameModel.RegisterActor adds every actor there, IDA); ActorModel.transform is the
// live object (null / destroyed while its scene is not loaded). Silo / vac contents are counts, not
// actors, so they are never in the lists.

#include "modules/slime_dj/plorts.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/log.h"
#include "core/module.h"
#include "core/perf.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/common/debug_marks.h"

namespace sr2::modules::slime_dj {

namespace {
namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
using il2cpp::api;
using il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr float kFindSeconds = 2.0f;          // plort list refresh (identifiablesByIdent), at most this often
constexpr float kNewSourceSeconds = 0.25f;    // a speaker started playing: refresh now, but at most this often
constexpr float kTypeRetrySeconds = 10.0f;    // plort types not found yet (groups not loaded)
constexpr float kListLookupSeconds = 10.0f;   // the kept plort lists are looked up again at least this often
// Glowing at once, nearest to a speaker first. Round 17 ("why not all?"): 64 -> 256. Per glowing plort
// and frame: one GetPropertyBlock, up to 9 Set*, one SetPropertyBlock per body renderer (native calls, a
// few microseconds), skipped while its look did not change (kSkip); frame:slime_dj was 0.05-0.1 ms with 64.
constexpr size_t kMaxPlorts = 256;
constexpr size_t kMaxModels = 1024;           // loose plorts read per refresh
constexpr size_t kMaxRends = 4;               // glowing renderers per plort (body + an attachment)
constexpr float kFullShare = 0.4f;            // full closeness up to this share of the range (as the lamps)
constexpr float kStack = 0.45f;               // strength = 1 - exp(-kStack x sum): one close speaker 0.36
// Look (the lamps' smoothing, lamps.cpp)
constexpr float kAttack = 0.015f, kRelease = 0.10f;  // bass level smoothing (s)
constexpr float kPulseDecay = 0.15f;                 // beat pulse time constant (s)
constexpr float kEnergyTime = 0.4f;                  // energy smoothing (s)
constexpr float kCalmShare = 0.15f;                  // level / pulse share left in the calmest part
constexpr float kLevel = 0.7f, kBeat = 1.5f;         // colour x (1 + level a + pulse b)
constexpr float kLift = 0.2f;                        // own-hue lift per unit of boost (dark plorts glow too)
constexpr float kRest = 0.004f;                      // below this a channel counts as settled
constexpr float kEase = 0.25f;                       // distance weight easing (s; the lamps' kFadeIn): a plort entering range fades in
constexpr float kSkip = 0.002f;                      // beat factor / fade change below this: the block is not rewritten
constexpr float kGlowFull = 0.8f;                    // Phosphor: beat factor - 1 at which its glow is at the material's max
// Debug highlighter
constexpr float kDebugFindSeconds = 4.0f, kDebugPublishSeconds = 1.0f;
constexpr size_t kDebugMaxIdle = 96;
constexpr uint32_t kColPulsing = 0xF040C0FF, kColIdle = 0xC0B0B0B0;  // IM_COL32 order (orange, grey)

struct Vec3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};
}  // namespace

struct PlortPulser::Impl {
    // ---- resolved
    bool ready = false;
    Il2CppClass* rendererClass = nullptr;
    Il2CppClass* groupClass = nullptr;
    Il2CppClass* identTypeClass = nullptr;
    Il2CppClass* actorModelClass = nullptr;
    Il2CppClass* mpbClass = nullptr;
    size_t offScGameModel = 0, offPlortGroup = 0, offMembers = 0, offSubGroups = 0, offByIdent = 0, offAmTransform = 0;
    const MethodInfo* sceneCtxInst = nullptr;     // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* mpbCtor = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    M<void (*)(Il2CppObject*, const MethodInfo*)> mpbClear;
    M<void (*)(Il2CppObject*, int32_t, Color, const MethodInfo*)> mpbSetColor;
    M<Color (*)(Il2CppObject*, int32_t, const MethodInfo*)> mpbGetColor;
    M<void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> rendGetBlock, rendSetBlock;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> rendSharedMaterial;
    M<bool (*)(Il2CppObject*, int32_t, const MethodInfo*)> matHasProperty;
    M<Color (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetColor;
    M<void (*)(Il2CppObject*, int32_t, float, const MethodInfo*)> mpbSetFloat;
    M<float (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetFloat;
    M<int32_t (*)(Il2CppString*, const MethodInfo*)> propertyToId;
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetPosition;
    Il2CppClass* dictClass = nullptr;  // the live Dictionary<IdentifiableType, List<IdentifiableModel>> class
    const MethodInfo* dictTryGet = nullptr;
    size_t offDictCount = 0;           // Dictionary._count (0 = unknown: lists looked up every time, no watch)
    Il2CppClass* listClass = nullptr;  // List<IdentifiableModel>
    size_t offListVersion = 0;
    // colours scaled by the beat; floats of the plort's own glow (replaced while it pulses)
    enum Prop { kTop, kMid, kBot, kTwinTop, kTwinMid, kTwinBot, kGlowTop, kProps };
    enum FProp { kPulseGlow, kGlowSpeed, kGlowMin, kGlowMax, kFProps };
    int32_t ids[kProps] = {}, fids[kFProps] = {};

    // ---- state
    struct RendRec {
        Il2CppGCHandle h = nullptr, saved = nullptr;  // saved: the renderer's own block
        bool has[kProps] = {};
        Color col[kProps] = {};
        bool fhas[kFProps] = {};
        float fval[kFProps] = {};
        bool phosphor = false;  // Plort (Glow): its glow animation follows the beat (_GlowTop left alone)
    };
    struct Plort {
        Il2CppGCHandle tr = nullptr;  // the plort's Transform (identity + alive check)
        Vec3 pos{};
        std::vector<RendRec> rends;
        const void* channel = nullptr;
        float weight = 0, shown = 0;
        float lastS = -1, lastFade = -1;  // look last written (Animate skips an unchanged one)
        bool touched = false;
    };
    // the plort lists as the last Find saw them (same order as `types`): kept while the dictionary's key
    // count stays the same, their _version watched by Changed()
    struct Watch {
        Il2CppGCHandle list = nullptr;  // nullptr = the type had no list then
        int version = 0;
    };
    std::vector<Watch> watch;
    Il2CppGCHandle watchDict = nullptr;
    int watchCount = 0;
    ULONGLONG lookupAt = 0;  // last TryGetValue round
    bool forceFind = false;  // Changed saw a change: the next Sync refreshes now
    struct ChannelState {
        uint64_t last = 0;
        unsigned gen = 0;
        float env = 0, pulse = 0, energy = 1;
        bool active = false, seen = false;
    };
    std::vector<Plort> plorts;
    std::unordered_map<const void*, ChannelState> channels;
    std::vector<Il2CppGCHandle> types;  // plort IdentifiableTypes
    ULONGLONG typeSearch = 0, lastFind = 0;
    bool typesLogged = false, typesMissingLogged = false, capLogged = false;
    Il2CppGCHandle work = nullptr;  // shared MaterialPropertyBlock
    std::vector<std::pair<Il2CppObject*, Vec3>> placed;  // Find: plort transforms (raw, valid during this call) + position
    size_t loaded = 0;
    bool wasOn = false;  // some plort glowed at the last Sync (log on a change)
    int builtLogs = 0;
    std::vector<std::pair<const void*, Vec3>> lastSources;
    // debug highlighter
    bool debugOn = false;
    ULONGLONG debugFind = 0, debugPublish = 0;
    std::vector<Il2CppGCHandle> debugIdle;
    std::string debugLogged;

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
            if (err.rfind("native exception", 0) == 0) core::MarkModuleFaulted(kModule, std::format("plorts: {}: {}", what, err));
            else SR2_WARN("slime_dj: plorts: {} failed: {}", what, err);
        }
        return r;
    }
    Il2CppObject* NewBlock() {
        Il2CppObject* b = objectNew(mpbClass);
        bool ok = false;
        if (b) Invoke(mpbCtor, b, {}, "MaterialPropertyBlock..ctor", &ok);
        return ok ? b : nullptr;
    }
    bool Position(Il2CppObject* tr, Vec3& out) {
        out = trGetPosition.fn(tr, trGetPosition.mi);
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }
    static bool Finite(const Color& c) { return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a); }

    Il2CppObject* GameModel() {
        bool ok = false;
        Il2CppObject* sc = Invoke(sceneCtxInst, nullptr, {}, "SceneContext.Instance", &ok);
        Il2CppObject* gm = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel) : nullptr;
        return unity::IsAlive(gm) ? gm : nullptr;
    }

    // ---- the plort types: GameModel.plortGroup and its sub-groups.
    bool Types(Il2CppObject* gm) {
        for (size_t i = 0; i < types.size();)
            if (Target(types[i])) {
                ++i;
            } else {
                Free(types[i]);
                types.erase(types.begin() + static_cast<std::ptrdiff_t>(i));
            }
        if (!types.empty()) return true;
        const ULONGLONG now = GetTickCount64();
        if (typeSearch && now - typeSearch < static_cast<ULONGLONG>(kTypeRetrySeconds * 1000)) return false;
        typeSearch = now;
        std::vector<Il2CppObject*> groups{unity::Field<Il2CppObject*>(gm, offPlortGroup)}, found;
        for (size_t gi = 0; gi < groups.size() && gi < 64; ++gi) {
            Il2CppObject* grp = groups[gi];
            if (!unity::IsAlive(grp) || !IsA(grp, groupClass)) continue;
            void* members = unity::Field<void*>(grp, offMembers);
            for (size_t i = 0, n = unity::ListCount(members); i < n && i < 500; ++i) {
                auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
                if (unity::IsAlive(t) && IsA(t, identTypeClass) && std::find(found.begin(), found.end(), t) == found.end()) found.push_back(t);
            }
            void* subs = unity::Field<void*>(grp, offSubGroups);
            for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
                groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
        }
        std::string names;
        for (Il2CppObject* t : found)
            if (Il2CppGCHandle h = unity::NewGCHandle(t)) {
                types.push_back(h);
                if (names.size() < 400) names += (names.empty() ? "" : ", ") + unity::GetName(t);
            }
        if (types.empty()) {
            if (!typesMissingLogged) {
                typesMissingLogged = true;
                SR2_WARN("slime_dj: plorts: GameModel.plortGroup has no plort types yet (retried every {:.0f} s)", kTypeRetrySeconds);
            }
            return false;
        }
        if (!typesLogged) {
            typesLogged = true;
            SR2_INFO("slime_dj: plorts: {} plort type(s): {}", types.size(), names);
        }
        return true;
    }

    // ---- every loose plort that is loaded now (transform + position).
    // The plort lists are looked up (one TryGetValue per type) only when the dictionary or its key count
    // changed, a type went away, or kListLookupSeconds passed; otherwise the kept lists are read directly.
    void Find() {
        SR2_PERF_DETAIL("slime_dj.plorts.find");
        placed.clear();
        Il2CppObject* gm = GameModel();
        if (!gm || !Types(gm)) return;
        Il2CppObject* dict = unity::Field<Il2CppObject*>(gm, offByIdent);
        if (!dict) return;
        if (Il2CppClass* k = unity::ClassOf(dict); k != dictClass) {
            dictClass = k;
            dictTryGet = k ? Resolver::Get().Method(k, "TryGetValue", 2, nullptr) : nullptr;
            const auto count = k ? Resolver::Get().FieldOffset(k, "_count", nullptr) : std::nullopt;
            offDictCount = count ? *count : 0;
            FreeWatch();
        }
        if (!dictTryGet) return;
        const ULONGLONG now = GetTickCount64();
        const bool keep = offDictCount && Managed(watchDict) == dict && unity::Field<int>(dict, offDictCount) == watchCount &&
                          watch.size() == types.size() && now - lookupAt < static_cast<ULONGLONG>(kListLookupSeconds * 1000);
        if (!keep) {
            std::vector<Il2CppObject*> lists;
            bool ok = false;
            for (Il2CppGCHandle h : types) {
                Il2CppObject* type = Target(h);
                Il2CppObject* list = nullptr;
                Il2CppObject* boxed = type ? Invoke(dictTryGet, dict, {type, &list}, "identifiablesByIdent.TryGetValue", &ok) : nullptr;
                if (type && !ok) return;
                const bool found = boxed && *reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
                lists.push_back(found ? list : nullptr);  // a managed List (no m_CachedPtr, so no IsAlive)
            }
            lookupAt = now;
            FreeWatch();
            watchDict = unity::NewGCHandle(dict);
            watchCount = offDictCount ? unity::Field<int>(dict, offDictCount) : 0;
            for (Il2CppObject* l : lists) {
                if (l && unity::ClassOf(l) != listClass) {
                    listClass = unity::ClassOf(l);
                    const auto version = Resolver::Get().FieldOffset(listClass, "_version", nullptr);
                    offListVersion = version ? *version : 0;
                }
                watch.push_back({l ? unity::NewGCHandle(l) : nullptr, 0});
            }
        }
        for (Watch& w : watch) {
            Il2CppObject* list = Managed(w.list);
            if (!list) continue;
            if (offListVersion) w.version = unity::Field<int>(list, offListVersion);
            for (size_t i = 0, n = unity::ListCount(list); i < n && placed.size() < kMaxModels; ++i) {
                auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
                if (!m || !IsA(m, actorModelClass)) continue;
                Il2CppObject* tr = unity::Field<Il2CppObject*>(m, offAmTransform);
                if (!unity::IsAlive(tr) || !unity::ActiveInHierarchy(unity::GameObjectOf(tr))) continue;
                Vec3 p;
                if (Position(tr, p)) placed.emplace_back(tr, p);
            }
        }
    }
    // managed collections (not UnityEngine.Object): the handle's target as is
    static Il2CppObject* Managed(Il2CppGCHandle h) { return h ? unity::GCHandleTarget(h) : nullptr; }
    void FreeWatch() {
        for (Watch& w : watch) Free(w.list);
        watch.clear();
        Free(watchDict);
        watchCount = 0;
    }
    // A plort list changed since Find read it (plain memory reads): a plort was made or went away.
    bool ListsChanged() {
        Il2CppObject* dict = Managed(watchDict);
        if (!dict || !offDictCount || !offListVersion) return false;
        if (unity::Field<int>(dict, offDictCount) != watchCount) return true;
        for (const Watch& w : watch)
            if (Il2CppObject* l = Managed(w.list))
                if (unity::Field<int>(l, offListVersion) != w.version) return true;
        return false;
    }

    std::vector<Il2CppObject*> Renderers(Il2CppObject* tr) {
        std::vector<Il2CppObject*> out;
        bool inactive = false, ok = false;
        Il2CppObject* arr = Invoke(compsInChildren, tr, {unity::TypeOf(rendererClass), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? std::min<size_t>(unity::ArrayLength(arr), 16) : 0; i < n; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (unity::IsAlive(c) && IsA(c, rendererClass)) out.push_back(c);
        }
        return out;
    }

    bool Build(Plort& plort, Il2CppObject* tr) {
        std::string what;
        for (Il2CppObject* r : Renderers(tr)) {
            if (plort.rends.size() >= kMaxRends) break;
            Il2CppObject* mat = rendSharedMaterial.fn(r, rendSharedMaterial.mi);
            if (!unity::IsAlive(mat)) continue;
            RendRec rec;
            for (int k = 0; k < kProps; ++k) rec.has[k] = ids[k] && matHasProperty.fn(mat, ids[k], matHasProperty.mi);
            if (!rec.has[kTop] || !rec.has[kMid] || !rec.has[kBot]) continue;  // the shield, quads, auras
            Il2CppObject* saved = NewBlock();
            if (!saved) continue;
            mpbClear.fn(saved, mpbClear.mi);
            rendGetBlock.fn(r, saved, rendGetBlock.mi);  // the renderer's own block, set back on restore
            for (int k = 0; k < kProps; ++k) {
                if (!rec.has[k]) continue;
                Color c = mpbGetColor.fn(saved, ids[k], mpbGetColor.mi);  // set by the game (a recolour)?
                if (!Finite(c) || (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0)) c = matGetColor.fn(mat, ids[k], matGetColor.mi);
                rec.has[k] = Finite(c);
                rec.col[k] = c;
            }
            // the plort's own glow (material values; the game does not set these through a block)
            for (int k = 0; k < kFProps; ++k) {
                rec.fhas[k] = fids[k] && matHasProperty.fn(mat, fids[k], matHasProperty.mi);
                if (rec.fhas[k]) rec.fval[k] = matGetFloat.fn(mat, fids[k], matGetFloat.mi);
                rec.fhas[k] = rec.fhas[k] && std::isfinite(rec.fval[k]);
            }
            rec.phosphor = rec.fhas[kGlowSpeed] && rec.fhas[kGlowMin] && rec.fhas[kGlowMax];
            if (rec.phosphor) rec.has[kGlowTop] = false;  // the beat drives the glow amount instead
            rec.h = unity::NewGCHandle(r);
            rec.saved = unity::NewGCHandle(saved);
            if (!rec.h || !rec.saved) {
                Free(rec.h);
                Free(rec.saved);
                continue;
            }
            if (what.size() < 200) {
                const Color& c = rec.col[kTop];
                what += std::format("{}'{}' top ({:.2f}, {:.2f}, {:.2f}){}{}{}", what.empty() ? "" : ", ", unity::GetName(mat), c.r, c.g, c.b,
                                    rec.fhas[kPulseGlow] ? std::format(" pulse glow {:.0f} off", rec.fval[kPulseGlow]) : "",
                                    rec.phosphor ? " +glow to the beat" : "", rec.has[kTwinTop] ? " +twin" : "");
            }
            plort.rends.push_back(rec);
        }
        if (plort.rends.empty()) return false;
        if (builtLogs++ < 6)
            SR2_INFO("slime_dj: plorts: glowing plort '{}' at ({:.0f}, {:.0f}, {:.0f}): {}", unity::GetName(tr), plort.pos.x, plort.pos.y,
                     plort.pos.z, what);
        return true;
    }

    void Restore(Plort& plort) {
        if (!plort.touched) return;
        plort.touched = false;
        plort.lastS = plort.lastFade = -1;
        for (const RendRec& r : plort.rends) {
            Il2CppObject* o = Target(r.h);
            Il2CppObject* saved = Target(r.saved);
            if (o && saved) rendSetBlock.fn(o, saved, rendSetBlock.mi);
        }
    }
    void Drop(Plort& plort) {
        Restore(plort);
        for (RendRec& r : plort.rends) {
            Free(r.h);
            Free(r.saved);
        }
        plort.rends.clear();
        Free(plort.tr);
    }

    void Animate(Plort& plort, const ChannelState& ch) {
        Il2CppObject* work0 = Target(work);
        if (!work0) return;
        const float e = ch.env * plort.shown, p = ch.pulse * plort.shown;
        const float s = 1.0f + kLevel * e + kBeat * p, lift = kLift * (s - 1.0f);
        // the plort's own glow fades out as the beat glow fades in (replaced, not added on top)
        const float fade = std::clamp(plort.shown / std::max(plort.weight, 1e-3f), 0.0f, 1.0f);
        if (plort.touched && std::fabs(s - plort.lastS) < kSkip && std::fabs(fade - plort.lastFade) < kSkip) return;  // unchanged
        plort.touched = true;
        plort.lastS = s;
        plort.lastFade = fade;
        const float amount = std::clamp((s - 1.0f) / kGlowFull, 0.0f, 1.0f);
        for (const RendRec& r : plort.rends) {
            Il2CppObject* o = Target(r.h);
            if (!o) continue;
            mpbClear.fn(work0, mpbClear.mi);
            rendGetBlock.fn(o, work0, rendGetBlock.mi);  // keeps whatever else the game put there
            for (int k = 0; k < kProps; ++k) {
                if (!r.has[k]) continue;
                const Color& c = r.col[k];
                const float m = std::max({c.r, c.g, c.b, 1e-3f});
                mpbSetColor.fn(work0, ids[k], Color{c.r * s + c.r / m * lift, c.g * s + c.g / m * lift, c.b * s + c.b / m * lift, c.a},
                               mpbSetColor.mi);
            }
            if (r.fhas[kPulseGlow]) mpbSetFloat.fn(work0, fids[kPulseGlow], r.fval[kPulseGlow] * (1.0f - fade), mpbSetFloat.mi);
            if (r.phosphor) {  // frozen animation, glow amount = the beat
                const float lo = std::min(r.fval[kGlowMin], r.fval[kGlowMax]), hi = std::max(r.fval[kGlowMin], r.fval[kGlowMax]);
                const float v = lo + (hi - lo) * amount;
                mpbSetFloat.fn(work0, fids[kGlowSpeed], 0.0f, mpbSetFloat.mi);
                mpbSetFloat.fn(work0, fids[kGlowMin], v, mpbSetFloat.mi);
                mpbSetFloat.fn(work0, fids[kGlowMax], v, mpbSetFloat.mi);
            }
            rendSetBlock.fn(o, work0, rendSetBlock.mi);
        }
    }
};

PlortPulser::PlortPulser() : p_(std::make_unique<Impl>()) {}
PlortPulser::~PlortPulser() = default;  // never runs for the module's instance (leaked on purpose)

bool PlortPulser::Ready() const { return p_->ready; }
size_t PlortPulser::Plorts() const { return p_->plorts.size(); }

bool PlortPulser::Resolve(std::string& why) {
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
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    P.identTypeClass = cls(kImage, "", "IdentifiableType");
    P.groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel");
    P.actorModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "ActorModel");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* shader = cls(kCore, "UnityEngine", "Shader");
    P.rendererClass = cls(kCore, "UnityEngine", "Renderer");
    P.mpbClass = cls(kCore, "UnityEngine", "MaterialPropertyBlock");
    if (!why.empty()) return false;
    off(sceneCtx, "<GameModel>k__BackingField", P.offScGameModel);
    off(gameModel, "plortGroup", P.offPlortGroup);
    off(gameModel, "identifiablesByIdent", P.offByIdent);
    off(P.groupClass, "_memberTypes", P.offMembers);
    off(P.groupClass, "_memberGroups", P.offSubGroups);
    off(P.actorModelClass, "transform", P.offAmTransform);
    if (!why.empty()) return false;
    P.sceneCtxInst = R.Method(parentOf(sceneCtx), "get_Instance", 0, nullptr);
    if (!P.sceneCtxInst) why = "SRSingleton<SceneContext>.get_Instance not found";
    P.compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    P.mpbCtor = byParams(P.mpbClass, ".ctor", {});
    bind(P.mpbClear, byParams(P.mpbClass, "Clear", {}), "MaterialPropertyBlock.Clear");
    bind(P.mpbSetColor, byParams(P.mpbClass, "SetColor", {"System.Int32", "UnityEngine.Color"}), "MaterialPropertyBlock.SetColor");
    bind(P.mpbGetColor, byParams(P.mpbClass, "GetColor", {"System.Int32"}), "MaterialPropertyBlock.GetColor");
    bind(P.rendGetBlock, byParams(P.rendererClass, "GetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.GetPropertyBlock");
    bind(P.rendSetBlock, byParams(P.rendererClass, "SetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.SetPropertyBlock");
    bind(P.rendSharedMaterial, byParams(P.rendererClass, "get_sharedMaterial", {}), "Renderer.get_sharedMaterial");
    bind(P.matHasProperty, byParams(material, "HasProperty", {"System.Int32"}), "Material.HasProperty");
    bind(P.matGetColor, byParams(material, "GetColor", {"System.Int32"}), "Material.GetColor");
    bind(P.mpbSetFloat, byParams(P.mpbClass, "SetFloat", {"System.Int32", "System.Single"}), "MaterialPropertyBlock.SetFloat");
    bind(P.matGetFloat, byParams(material, "GetFloat", {"System.Int32"}), "Material.GetFloat");
    bind(P.propertyToId, byParams(shader, "PropertyToID", {"System.String"}), "Shader.PropertyToID");
    bind(P.trGetPosition, byParams(transform, "get_position", {}), "Transform.get_position");
    if (!why.empty()) return false;
    P.ready = true;
    return true;
}

void PlortPulser::Sync(const std::vector<Source>& sources, float range) {
    Impl& P = *p_;
    if (!P.ready) return;
    if (!P.ids[Impl::kTop]) {  // main thread, first use
        static constexpr const char* kNames[Impl::kProps] = {"_TopColor",        "_MiddleColor",     "_BottomColor", "_TwinTopColor",
                                                             "_TwinMiddleColor", "_TwinBottomColor", "_GlowTop"};
        static constexpr const char* kFNames[Impl::kFProps] = {"_PulseGlowIntensity", "_GlowSpeed", "_GlowMin", "_GlowMax"};
        for (int k = 0; k < Impl::kProps; ++k) P.ids[k] = P.propertyToId.fn(unity::NewString(kNames[k]), P.propertyToId.mi);
        for (int k = 0; k < Impl::kFProps; ++k) P.fids[k] = P.propertyToId.fn(unity::NewString(kFNames[k]), P.propertyToId.mi);
    }
    if (!Impl::Target(P.work)) {
        Impl::Free(P.work);
        if (Il2CppObject* b = P.NewBlock()) P.work = unity::NewGCHandle(b);
    }
    range = std::max(range, 1.0f);
    // Candidates: every loaded plort at a refresh (at most every kFindSeconds, within kNewSourceSeconds when
    // a speaker joined); between refreshes the plorts already held, at their current positions.
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
    } else if (P.forceFind || !P.lastFind || now - P.lastFind >= every) {  // forceFind: a plort list changed (Changed)
        P.lastFind = now;
        P.forceFind = false;
        P.Find();
        P.loaded = P.placed.size();
    } else {
        for (const Impl::Plort& pl : P.plorts) {
            Il2CppObject* tr = Impl::Target(pl.tr);
            Vec3 pos;
            if (tr && unity::ActiveInHierarchy(unity::GameObjectOf(tr)) && P.Position(tr, pos)) P.placed.emplace_back(tr, pos);
        }
    }
    // Which plorts are in range of a playing speaker: nearest speaker's channel, strength from that
    // channel's speakers in range (the lamps' rule).
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
        float sum = 0;
        for (const Source& s : sources) {
            if (s.channel != ch) continue;
            const float dx = s.x - pos.x, dy = s.y - pos.y, dz = s.z - pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            sum += std::clamp((range - d) / (range * (1.0f - kFullShare)), 0.0f, 1.0f) * std::max(s.loud, 0.0f);
        }
        const float w = std::clamp(1.0f - std::exp(-kStack * sum), 0.0f, 1.0f);
        if (w > 0.0f) want.push_back({tr, pos, ch, w, best});
    }
    const size_t inRange = want.size();
    if (want.size() > kMaxPlorts) {
        std::partial_sort(want.begin(), want.begin() + kMaxPlorts, want.end(), [](const Want& a, const Want& b) { return a.d2 < b.d2; });
        want.resize(kMaxPlorts);
        if (!P.capLogged) {
            P.capLogged = true;
            SR2_INFO("slime_dj: plorts: {} plort(s) in range, only the {} nearest to a speaker glow", inRange, kMaxPlorts);
        }
    }
    for (size_t i = 0; i < P.plorts.size();) {
        Impl::Plort& pl = P.plorts[i];
        Il2CppObject* tr = Impl::Target(pl.tr);
        auto it = tr ? std::find_if(want.begin(), want.end(), [&](const Want& w) { return w.tr == tr; }) : want.end();
        if (it == want.end()) {
            P.Drop(pl);
            P.plorts.erase(P.plorts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (pl.channel != it->channel) P.Restore(pl);  // another song now: start from the plort's own look
        pl.channel = it->channel;
        pl.pos = it->pos;
        pl.weight = it->weight;
        ++i;
    }
    for (const Want& w : want) {
        if (std::any_of(P.plorts.begin(), P.plorts.end(), [&](const Impl::Plort& pl) { return Impl::Target(pl.tr) == w.tr; })) continue;
        Impl::Plort pl;
        pl.pos = w.pos;
        pl.channel = w.channel;
        pl.weight = w.weight;
        pl.tr = unity::NewGCHandle(w.tr);
        if (!pl.tr) continue;
        if (!P.Build(pl, w.tr)) {
            P.Drop(pl);
            continue;
        }
        P.plorts.push_back(std::move(pl));
    }
    // Logged when plorts start or stop glowing (the count itself changes all the time: plorts are made,
    // eaten, sold and vacuumed).
    if (P.plorts.empty() == P.wasOn) {
        P.wasOn = !P.plorts.empty();
        SR2_INFO("slime_dj: plorts glow: {} plort(s) ({} loose plort(s) loaded, {} playing speaker(s), range {:.0f} m)", P.plorts.size(),
                 P.loaded, sources.size(), range);
    }
    P.placed.clear();
}

bool PlortPulser::Changed() {
    Impl& P = *p_;
    if (!P.ready || P.lastSources.empty()) return false;  // no speaker plays: the next one refreshes anyway
    if (P.forceFind) return true;  // still pending (a Find of the debug highlighter saw the change)
    if (!P.ListsChanged()) return false;
    P.forceFind = true;
    return true;
}

void PlortPulser::Tick(float dt, const std::vector<Channel>& channels) {
    Impl& P = *p_;
    if (!P.ready) return;
    SR2_PERF_DETAIL("slime_dj.plorts.tick");
    dt = std::clamp(dt, 0.0f, 0.1f);
    for (auto& [key, st] : P.channels) st.seen = st.active = false;
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
        if (beat && beat->beat > 0) st.pulse = std::max(st.pulse, beat->beat * gain);
    }
    std::erase_if(P.channels, [&](const auto& e) {
        return !e.second.seen && std::none_of(P.plorts.begin(), P.plorts.end(), [&](const Impl::Plort& pl) { return pl.channel == e.first; });
    });
    for (size_t i = 0; i < P.plorts.size();) {
        Impl::Plort& pl = P.plorts[i];
        if (!Impl::Target(pl.tr)) {  // vacuumed, sold, eaten or unloaded: nothing to restore
            pl.touched = false;
            P.Drop(pl);
            P.plorts.erase(P.plorts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        auto it = P.channels.find(pl.channel);
        Impl::ChannelState* st = it != P.channels.end() && it->second.seen ? &it->second : nullptr;
        pl.shown += (pl.weight - pl.shown) * (1.0f - std::exp(-dt / kEase));
        const bool settled = !st || (!st->active && st->env < kRest && st->pulse < kRest);
        if (settled) P.Restore(pl);
        else P.Animate(pl, *st);
        ++i;
    }
}

// Debug highlighter (modules/common/debug_marks.h): every kDebugFindSeconds a plort refresh of its own
// (also without music), then boxes at the current positions of the glowing plorts and of up to
// kDebugMaxIdle loaded plorts that do not glow.
void PlortPulser::Debug(bool want) {
    Impl& P = *p_;
    namespace dm = common::debug_marks;
    if (!P.ready) return;
    auto freeIdle = [&] {
        for (Il2CppGCHandle& h : P.debugIdle) Impl::Free(h);
        P.debugIdle.clear();
    };
    if (!want) {
        if (P.debugOn) {
            P.debugOn = false;
            freeIdle();
            dm::Remove("slime_dj.plorts.glowing");
            dm::Remove("slime_dj.plorts.idle");
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!P.debugOn) {
        P.debugOn = true;
        P.debugFind = P.debugPublish = 0;
        P.debugLogged.clear();
    }
    if (!P.debugFind || now - P.debugFind >= static_cast<ULONGLONG>(kDebugFindSeconds * 1000)) {
        P.debugFind = now;
        P.debugPublish = 0;
        if (P.ListsChanged()) P.forceFind = true;  // this Find must not swallow a change the glow has not seen
        P.Find();
        P.loaded = P.placed.size();
        freeIdle();
        for (size_t i = 0; i < P.placed.size() && P.debugIdle.size() < kDebugMaxIdle; ++i)
            if (Il2CppGCHandle h = unity::NewGCHandle(P.placed[i].first)) P.debugIdle.push_back(h);
        P.placed.clear();
    }
    if (P.debugPublish && now - P.debugPublish < static_cast<ULONGLONG>(kDebugPublishSeconds * 1000)) return;
    P.debugPublish = now;
    auto box = [](const Vec3& p) {
        dm::Box b;
        b.x = p.x, b.y = p.y, b.z = p.z;
        b.hx = b.hy = b.hz = 0.3f;
        return b;
    };
    std::vector<Il2CppObject*> glowingObjs;
    std::vector<dm::Box> glowing, idle;
    for (const Impl::Plort& pl : P.plorts)
        if (Il2CppObject* tr = Impl::Target(pl.tr)) {
            Vec3 p;
            if (!P.Position(tr, p)) continue;
            glowingObjs.push_back(tr);
            glowing.push_back(box(p));
        }
    for (Il2CppGCHandle h : P.debugIdle) {
        Il2CppObject* tr = Impl::Target(h);
        Vec3 p;
        if (tr && std::find(glowingObjs.begin(), glowingObjs.end(), tr) == glowingObjs.end() &&
            unity::ActiveInHierarchy(unity::GameObjectOf(tr)) && P.Position(tr, p))
            idle.push_back(box(p));
    }
    const std::string summary = std::format("{} glowing, {} not glowing ({} loose plort(s) loaded)", glowing.size(), idle.size(), P.loaded);
    if (summary != P.debugLogged) {
        P.debugLogged = summary;
        SR2_INFO("slime_dj: plorts: highlighter: {}", summary);
    }
    dm::Publish("slime_dj.plorts.glowing", "glowing plort", kColPulsing, std::move(glowing));
    dm::Publish("slime_dj.plorts.idle", "plort (no playing speaker near)", kColIdle, std::move(idle));
}

void PlortPulser::Clear() {
    Impl& P = *p_;
    for (Impl::Plort& pl : P.plorts) P.Drop(pl);
    if (!P.plorts.empty()) SR2_INFO("slime_dj: plorts glow stopped; plorts restored");
    P.plorts.clear();
    P.placed.clear();
    P.channels.clear();
    P.lastSources.clear();
    P.lastFind = 0;
    P.forceFind = false;
    P.wasOn = false;
}

void RegisterPlortSettings() {
    using settings::Kind;
    settings::Def x;
    x.module = kModule;
    x.key = "dance_plorts";
    x.label = "Plorts glow to the music";
    x.kind = Kind::Bool;
    x.def = 1;
    x.min = 0;
    x.max = 1;
    x.step = 1;
    x.tooltip = "Loose plorts near a music box, DJ Booth, speaker stack or radio that plays a song glow brighter "
                "with the beat, in their own colours: calm in quiet parts, flashing in loud ones. More speakers "
                "make them glow stronger; the beat replaces their own glow while they dance. Up to 256 plorts at once "
                "(the nearest to a speaker first), so a ranch full of plorts stays smooth. They go back to "
                "normal when the song stops or pauses.";
    settings::Register(x);
    x.key = "plort_range";
    x.label = "Plort range";
    x.kind = Kind::Float;
    x.def = 15.0;
    x.min = 5.0;
    x.max = 60.0;
    x.step = 5.0;
    x.parent = "dance_plorts";
    x.tooltip = "The distance from a speaker (music box, DJ Booth, speaker stack or radio) within which "
                "plorts glow while it plays. Strongest close to the speaker, fading out towards this distance.";
    settings::Register(x);
    settings::SetDisplay(kModule, "plort_range", " m");
}

}  // namespace sr2::modules::slime_dj
