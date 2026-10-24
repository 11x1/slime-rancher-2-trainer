// custom_upgrades: the mod's buildables for other modules (see buildables.h). The data lives in
// quantum_link.cpp and dj_buildables.cpp; this file only routes by id and keeps the override keys.

#include "modules/custom_upgrades/buildables.h"

#include <algorithm>
#include <format>

#include "core/log.h"
#include "core/main_thread.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/dj_buildables.h"
#include "modules/custom_upgrades/quantum_link.h"

namespace sr2::modules::custom_upgrades::buildables {
namespace {

namespace main_thread = sr2::core::main_thread;

bool Known(const std::string& id) {
    return id == kQuantumLink || id == kDjBooth || id == kDjSpeaker || id == kSlimeRadio;
}

bool Usable(std::string& result) {
    if (!main_thread::IsMainThread()) {
        result = "not on the game's main thread";
        return false;
    }
    if (!Active()) {
        result = "custom upgrades are off";
        return false;
    }
    return true;
}

const char* UnlockText(Unlock u) {
    switch (u) {
        case Unlock::kUnlocked: return "unlocked (override)";
        case Unlock::kLocked: return "locked (override)";
        default: return "follows the base blueprint";
    }
}

}  // namespace

std::string UnlockKey(const std::string& id) { return "buildables.unlock." + id; }

Unlock ParseUnlock(const std::string& value) {
    if (value == "1") return Unlock::kUnlocked;
    if (value == "0") return Unlock::kLocked;
    return Unlock::kFollowBase;
}

std::vector<Info> List() {
    std::vector<Info> out;
    std::string why;
    if (!Usable(why)) return out;
    Info q;
    if (quantum::Describe(q)) out.push_back(std::move(q));
    dj::Describe(out);
    return out;
}

bool Grant(const std::string& id, int count, std::string& result) {
    result.clear();
    if (!Known(id)) {
        result = std::format("unknown buildable '{}'", id);
        return false;
    }
    if (!Usable(result)) return false;
    if (count < 1) {
        result = "nothing to give (count < 1)";
        return false;
    }
    const bool ok = id == kQuantumLink ? quantum::Grant(count, result) : dj::Grant(id, count, result);
    if (ok) SR2_INFO("[custom_upgrades] buildables: granted {}: {}", id, result);
    else SR2_WARN("[custom_upgrades] buildables: {} not granted: {}", id, result);
    return ok;
}

bool SetUnlock(const std::string& id, Unlock unlock, std::string& result) {
    result.clear();
    if (!Known(id)) {
        result = std::format("unknown buildable '{}'", id);
        return false;
    }
    if (!Usable(result)) return false;
    const std::string value = unlock == Unlock::kUnlocked ? "1" : unlock == Unlock::kLocked ? "0" : "";
    if (!SetSaveValue(UnlockKey(id), value)) {
        result = "no save game is loaded";
        return false;
    }
    result = std::format("{}: Fabricator card {}", id, UnlockText(unlock));
    SR2_INFO("[custom_upgrades] buildables: {} (sidecar '{}' = '{}', written with the next game save)", result, UnlockKey(id),
             value);
    return true;
}

}  // namespace sr2::modules::custom_upgrades::buildables
