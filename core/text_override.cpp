#include "text_override.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace sr2::core::text_override {
namespace {

std::mutex g_mutex;
std::unordered_map<Il2CppObject*, std::string> g_texts;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sink{false};

}  // namespace

void Set(Il2CppObject* localizedString, std::string text) {
    if (!localizedString) return;
    std::lock_guard lock(g_mutex);
    g_texts[localizedString] = std::move(text);
    g_count.store(g_texts.size(), std::memory_order_release);
}

void Remove(Il2CppObject* localizedString) {
    std::lock_guard lock(g_mutex);
    g_texts.erase(localizedString);
    g_count.store(g_texts.size(), std::memory_order_release);
}

bool Lookup(Il2CppObject* localizedString, std::string& out) {
    if (!localizedString || g_count.load(std::memory_order_acquire) == 0) return false;
    std::lock_guard lock(g_mutex);
    auto it = g_texts.find(localizedString);
    if (it == g_texts.end()) return false;
    out = it->second;
    return true;
}

bool Any() { return g_count.load(std::memory_order_acquire) != 0; }

void SetSinkActive(bool active) { g_sink.store(active, std::memory_order_release); }
bool SinkActive() { return g_sink.load(std::memory_order_acquire); }

}  // namespace sr2::core::text_override
