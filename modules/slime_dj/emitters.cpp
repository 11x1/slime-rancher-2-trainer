// slime_dj: emitter registry (api.h / emitters.h). Other modules register GameObjects here; the
// slime_dj tick reads the list every box scan and gives each live entry a speaker.

#include "modules/slime_dj/emitters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "core/log.h"
#include "core/unity.h"

namespace sr2::modules::slime_dj {
namespace {

namespace unity = sr2::core::unity;

std::mutex g_mu;
std::vector<emitters::Entry> g_live;     // guarded by g_mu
std::vector<Il2CppGCHandle> g_toFree;    // guarded by g_mu; freed on the slime_dj tick
std::atomic<unsigned> g_gen{1};
std::atomic<bool> g_playing{false};
std::mutex g_nameMu;
std::string g_name;  // guarded by g_nameMu

float Clamp(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

}  // namespace

bool RegisterEmitter(Il2CppObject* obj, EmitterKind kind, float rangeScale, float volumeScale) {
    if (!unity::IsAlive(obj)) return false;
    const float range = Clamp(rangeScale, 0.1f, 4.0f, 1.0f), volume = Clamp(volumeScale, 0.0f, 2.0f, 1.0f);
    std::lock_guard lock(g_mu);
    for (emitters::Entry& e : g_live) {
        if (e.key != obj) continue;
        e.kind = kind;
        e.range = range;
        e.volume = volume;
        g_gen.fetch_add(1);
        return true;
    }
    if (g_live.size() >= 256) {
        static bool logged = false;
        if (!logged) SR2_WARN("slime_dj: more than 256 emitters registered; ignoring the rest");
        logged = true;
        return false;
    }
    emitters::Entry e;
    e.key = obj;
    e.h = unity::NewGCHandle(obj);
    if (!e.h) return false;
    e.kind = kind;
    e.range = range;
    e.volume = volume;
    g_live.push_back(e);
    g_gen.fetch_add(1);
    return true;
}

void UnregisterEmitter(Il2CppObject* obj) {
    if (!obj) return;
    std::lock_guard lock(g_mu);
    for (size_t i = 0; i < g_live.size(); ++i) {
        if (g_live[i].key != obj) continue;
        g_toFree.push_back(g_live[i].h);
        g_live.erase(g_live.begin() + static_cast<std::ptrdiff_t>(i));
        g_gen.fetch_add(1);
        return;
    }
}

bool SongPlaying() { return g_playing.load(std::memory_order_relaxed); }

std::string SongName() {
    std::lock_guard lock(g_nameMu);
    return g_name;
}

namespace emitters {

std::vector<Entry> Collect() {
    std::vector<Il2CppGCHandle> drop;
    std::vector<Entry> out;
    {
        std::lock_guard lock(g_mu);
        drop.swap(g_toFree);
        for (size_t i = 0; i < g_live.size();) {
            Il2CppObject* o = unity::GCHandleTarget(g_live[i].h);
            if (!unity::IsAlive(o)) {  // destroyed by Unity (plot unloaded, prop removed, ...)
                drop.push_back(g_live[i].h);
                g_live.erase(g_live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            out.push_back(g_live[i]);
            ++i;
        }
    }
    for (Il2CppGCHandle h : drop)
        if (h) unity::FreeGCHandle(h);
    return out;
}

unsigned Generation() { return g_gen.load(); }

void SetPlaying(bool playing, const std::string& name) {
    g_playing.store(playing, std::memory_order_relaxed);
    std::lock_guard lock(g_nameMu);
    if (g_name != name) g_name = name;
}

}  // namespace emitters
}  // namespace sr2::modules::slime_dj
