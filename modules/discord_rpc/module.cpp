// discord_rpc: Discord Rich Presence without the Discord SDK (see NOTES.md).
//
// Main thread: 4 times a second a frame callback reads where the player is (the
// current zone's localized name, whether it is the ranch, optionally the
// Newbucks) and hands a Presence to ipc.cpp. The pipe thread there talks to the
// Discord client; nothing on the main thread waits for it.
//
// Default OFF. Uses Slime Rancher 2's own Discord application id by default
// ("official_app"), else the user's own (sr2mods.json
// "modules.discord_rpc.application_id", edited on the overlay's Settings page).

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"  // ModuleFaulted / MarkModuleFaulted
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/discord_rpc/ipc.h"
#include "ui/discord_bridge.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace settings = sr2::core::settings;
namespace bridge = sr2::ui::discord;
namespace rpc = sr2::modules::discord_rpc;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = bridge::kModule;
constexpr const char* kImage = "Assembly-CSharp";
constexpr unsigned long long kReadMs = 250;  // 4 Hz: a handful of runtime_invoke calls, Publish only signals on change
constexpr int kNewbuckPersistenceId = 1;  // CurrencyUtility.NEWBUCK_PERSISTENCE_ID

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

std::string Thousands(int64_t v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

std::string CleanText(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>' && tag) tag = false;
        else if (!tag && c != '\r' && c != '\n') out += c;
    }
    const size_t a = out.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return out.substr(a, out.find_last_not_of(" \t") - a + 1);
}

class DiscordRpc final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    bool SetupWhileDisabled() const override { return true; }  // registers the settings for the Settings card
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

private:
    struct Zone {
        std::string name;  // localized, else the name key
        bool ranch = false;
    };

    bool Live() const { return enabled_.load() && !sr2::core::ModuleFaulted(kModule); }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr);
    template <class T>
    bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
        bool ok = false;
        Il2CppObject* r = Call(m, obj, args, what, &ok);
        if (!ok || !r) return false;
        out = *reinterpret_cast<T*>(reinterpret_cast<char*>(r) + 2 * sizeof(void*));
        return true;
    }
    void Tick();
    rpc::Presence Read();
    const Zone* ZoneInfo(Il2CppObject* zone);
    bool Newbucks(Il2CppObject* player, int& out);

    std::atomic<bool> enabled_{false};
    int frameCb_ = 0;
    unsigned long long lastRead_ = 0;
    int64_t sessionStart_ = 0;  // unix seconds the save was loaded (0 = in the menu)
    std::unordered_map<uintptr_t, Zone> zones_;  // ZoneDefinition identity -> info (assets stay loaded)
    std::string gameWhy_;  // "" = game state readable (else the presence only says "Playing")

    Il2CppClass* playerStateClass_ = nullptr;
    Il2CppClass* zoneClass_ = nullptr;
    Il2CppClass* locStringClass_ = nullptr;
    Il2CppClass* currencyClass_ = nullptr;
    const MethodInfo* tryGetPlayerState_ = nullptr;   // SceneContextUtility.TryGetPlayerState(out PlayerState)
    const MethodInfo* tryGetSceneContext_ = nullptr;  // SceneContextUtility.TryGetSceneContext(out SceneContext)
    const MethodInfo* scZoneTracker_ = nullptr;       // SceneContext.get_PlayerZoneTracker
    const MethodInfo* currentZone_ = nullptr;         // PlayerZoneTracker.GetCurrentZone
    const MethodInfo* latestZone_ = nullptr;          // PlayerZoneTracker.GetLatestZone (last non-null zone)
    size_t offZoneNameKey_ = 0;                       // ZoneDefinition._nameKey (string)
    size_t offZoneLocName_ = 0;                       // ZoneDefinition._localizedName (LocalizedString)
    size_t offZoneRichId_ = 0;                        // ZoneDefinition._richPresenceId (string)
    const MethodInfo* locGet_ = nullptr;              // LocalizedString.GetLocalizedString()
    const MethodInfo* defaultCurrency_ = nullptr;     // CurrencyUtility.get_DefaultCurrency (optional, Newbucks)
    const MethodInfo* getCurrency_ = nullptr;         // PlayerState.GetCurrency(ICurrency)
    size_t offPersistenceId_ = 0;                     // CurrencyDefinition._persistenceId
};

Il2CppObject* DiscordRpc::Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

bool DiscordRpc::Setup(sr2::core::ModuleContext& ctx) {
    {
        settings::Def d;
        d.module = kModule;
        d.kind = settings::Kind::Bool;
        d.key = bridge::kKeyOfficial;
        d.label = "Show as Slime Rancher 2";
        d.def = 1;
        d.tooltip = "Use Slime Rancher 2's own Discord application (its official id), so your profile shows "
                    "\"Playing Slime Rancher 2\" with the game's icon and you need no application of your own. "
                    "Off: your own application id below.";
        settings::Register(d);
        d.key = bridge::kKeyZone;
        d.label = "Show the area";
        d.def = 1;
        d.tooltip = "Show the zone you are in (for example \"Rainbow Fields\").";
        settings::Register(d);
        d.key = bridge::kKeyNewbucks;
        d.label = "Show Newbucks";
        d.def = 0;
        d.tooltip = "Add your current Newbucks to the second line.";
        settings::Register(d);
        d.key = bridge::kKeyMemberList;
        d.label = "Member list shows";
        d.kind = settings::Kind::Enum;
        d.def = 1;
        d.min = 0;
        d.max = 2;
        d.step = 1;
        d.options = {"Game name", "Area", "Activity"};
        d.tooltip = "What the server member list and the friends list show after \"Playing\": the game's (the "
                    "application's) name, the first line (the area) or the second line (\"On the ranch\", "
                    "\"Exploring\"). Needs a recent Discord app; older ones always show the name.";
        settings::Register(d);
    }

    // Game state. Missing symbols don't fail the module: the presence then shows "Playing" only.
    auto& R = ctx.resolver();
    std::string& w = gameWhy_;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && w.empty()) w = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (w.empty()) w = err.empty() ? std::format("field {} not found", field) : err;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        if (!k) return nullptr;
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (w.empty()) w = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    playerStateClass_ = cls(kImage, "", "PlayerState");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* tracker = cls(kImage, "", "PlayerZoneTracker");
    zoneClass_ = cls(kImage, "MonomiPark.SlimeRancher.World", "ZoneDefinition");
    locStringClass_ = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    tryGetPlayerState_ = method(scu, "TryGetPlayerState", 1);
    tryGetSceneContext_ = method(scu, "TryGetSceneContext", 1);
    scZoneTracker_ = method(sceneCtx, "get_PlayerZoneTracker", 0);
    currentZone_ = method(tracker, "GetCurrentZone", 0);
    latestZone_ = method(tracker, "GetLatestZone", 0);
    off(zoneClass_, "_nameKey", offZoneNameKey_);
    off(zoneClass_, "_localizedName", offZoneLocName_);
    off(zoneClass_, "_richPresenceId", offZoneRichId_);
    locGet_ = method(locStringClass_, "GetLocalizedString", 0);
    if (!w.empty()) SR2_WARN("[{}] game state unavailable ({}): the presence only shows \"Playing\"", kModule, w);
    {
        std::string cw;
        std::swap(cw, w);  // reuse the helpers for the optional Newbucks part
        currencyClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyDefinition");
        Il2CppClass* util = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyUtility");
        defaultCurrency_ = method(util, "get_DefaultCurrency", 0);
        getCurrency_ = method(playerStateClass_, "GetCurrency", 1);
        off(currencyClass_, "_persistenceId", offPersistenceId_);
        if (!w.empty()) {
            SR2_WARN("[{}] Newbucks unavailable: {}", kModule, w);
            defaultCurrency_ = getCurrency_ = nullptr;
        }
        std::swap(cw, w);
    }

    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float) { Tick(); });
    if (!frameCb_) return ctx.Fail("could not add the main-thread frame callback");
    SR2_INFO("[{}] setup ok ({}application id: {})", kModule, gameWhy_.empty() ? "" : "game state off, ",
             settings::GetBool(kModule, bridge::kKeyOfficial)                ? "Slime Rancher 2's own"
             : settings::GetString(kModule, bridge::kKeyAppId).empty() ? "own, not set"
                                                                         : "own");
    return true;
}

void DiscordRpc::OnEnable() {
    enabled_.store(true);
    lastRead_ = 0;
    rpc::SetActive(true);
    SR2_INFO("[{}] enabled", kModule);
}

void DiscordRpc::OnDisable() {
    enabled_.store(false);
    rpc::SetActive(false);  // the pipe thread clears the presence and closes the pipe
    SR2_INFO("[{}] disabled", kModule);
}

const DiscordRpc::Zone* DiscordRpc::ZoneInfo(Il2CppObject* zone) {
    const auto key = reinterpret_cast<uintptr_t>(zone);
    if (auto it = zones_.find(key); it != zones_.end()) return &it->second;
    if (!IsA(zone, zoneClass_) || !unity::IsAlive(zone)) return nullptr;
    Zone z;
    const std::string nameKey = unity::ToUtf8(unity::Field<Il2CppString*>(zone, offZoneNameKey_));
    const std::string richId = unity::ToUtf8(unity::Field<Il2CppString*>(zone, offZoneRichId_));
    if (auto* ls = unity::Field<Il2CppObject*>(zone, offZoneLocName_); ls && IsA(ls, locStringClass_))
        if (Il2CppObject* r = Call(locGet_, ls, {}, "LocalizedString.GetLocalizedString"))
            z.name = CleanText(unity::ToUtf8(reinterpret_cast<Il2CppString*>(r)));
    if (z.name.empty()) z.name = nameKey;
    // The ranch (the Conservatory) by its name key / rich-presence id; logged once per zone so it can be checked.
    std::string probe = nameKey + " " + richId;
    for (char& c : probe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    z.ranch = probe.find("ranch") != std::string::npos || probe.find("conservatory") != std::string::npos;
    SR2_INFO("[{}] zone \"{}\" (name key \"{}\", rich presence id \"{}\"){}", kModule, z.name, nameKey, richId,
             z.ranch ? ": the ranch" : "");
    auto& slot = zones_[key];
    slot = std::move(z);
    return &slot;
}

bool DiscordRpc::Newbucks(Il2CppObject* player, int& out) {
    if (!defaultCurrency_ || !getCurrency_) return false;
    bool ok = false;
    Il2CppObject* cur = Call(defaultCurrency_, nullptr, {}, "CurrencyUtility.DefaultCurrency", &ok);
    if (!ok || !IsA(cur, currencyClass_) || !unity::IsAlive(cur) ||
        unity::Field<int32_t>(cur, offPersistenceId_) != kNewbuckPersistenceId)
        return false;
    return CallValue(getCurrency_, player, {cur}, "PlayerState.GetCurrency", out);
}

rpc::Presence DiscordRpc::Read() {
    rpc::Presence p;
    // Member list: Game name / Area / Activity -> status_display_type 0 name, 2 details, 1 state.
    static constexpr int kDisplayType[] = {0, 2, 1};
    const int ml = settings::GetInt(kModule, bridge::kKeyMemberList);
    p.displayType = kDisplayType[ml < 0 || ml > 2 ? 0 : ml];
    Il2CppObject* player = nullptr;
    bool found = false;
    const bool inGame = gameWhy_.empty() &&
                        CallValue(tryGetPlayerState_, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) &&
                        found && IsA(player, playerStateClass_) && unity::IsAlive(player);
    if (!gameWhy_.empty()) {
        p.details = "Playing";
        return p;
    }
    if (!inGame) {
        sessionStart_ = 0;
        p.details = "In the main menu";
        return p;
    }
    if (!sessionStart_) sessionStart_ = static_cast<int64_t>(std::time(nullptr));
    p.inGame = true;
    p.start = sessionStart_;
    const Zone* zone = nullptr;
    Il2CppObject* sc = nullptr;
    if (CallValue(tryGetSceneContext_, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) && found && sc)
        if (Il2CppObject* tr = Call(scZoneTracker_, sc, {}, "SceneContext.PlayerZoneTracker"); tr && unity::IsAlive(tr)) {
            Il2CppObject* z = Call(currentZone_, tr, {}, "PlayerZoneTracker.GetCurrentZone");
            if (!z) z = Call(latestZone_, tr, {}, "PlayerZoneTracker.GetLatestZone");
            if (z) zone = ZoneInfo(z);
        }
    const bool showZone = settings::GetBool(kModule, bridge::kKeyZone);
    p.details = showZone && zone ? zone->name : "Playing";
    p.state = zone && zone->ranch ? "On the ranch" : "Exploring";
    int bucks = 0;
    if (settings::GetBool(kModule, bridge::kKeyNewbucks) && Newbucks(player, bucks))
        p.state += std::format(" \xC2\xB7 {} Newbucks", Thousands(bucks));
    return p;
}

void DiscordRpc::Tick() {
    if (!Live()) return;
    const unsigned long long now = GetTickCount64();
    if (now - lastRead_ < kReadMs) return;
    lastRead_ = now;
    rpc::Publish(Read());
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateDiscordRpc() { return std::make_unique<DiscordRpc>(); }

}  // namespace sr2::modules
