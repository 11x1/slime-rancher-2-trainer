// autotest: the generic probes and actions of wave 1 (NOTES.md "Generic probes and actions"). They reuse
// the Sandbox (its bridges: the same requests the Sandbox page sends), custom_upgrades' public API and
// the game's own calls; nothing is copied from those modules.

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>

#include "core/log.h"
#include "core/module_manager.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/autotest/api.h"
#include "modules/autotest/game.h"
#include "modules/custom_upgrades/api.h"
#include "ui/sandbox_bridge.h"
#include "ui/sandbox_upgrades_bridge.h"

namespace sr2::modules::autotest {

void RegisterGenericActions();

namespace {

namespace unity = sr2::core::unity;
namespace sb = sr2::ui::sandbox;
namespace ub = sr2::ui::sandbox::upgrades;
namespace cu = sr2::modules::custom_upgrades;
using game::Call;
using game::CallValue;

constexpr const char* kImage = "Assembly-CSharp";
constexpr int kSnapshotWaitTries = 50;  // x ~100 ms

std::string Norm(const std::string& s) {
    std::string r;
    for (unsigned char c : s)
        if (std::isalnum(c)) r += static_cast<char>(std::tolower(c));
    return r;
}

void NeedSandbox() {
    if (!sr2::core::ModuleManager::Get().IsEnabled("sandbox"))
        throw Error("the Sandbox is off: settings_set sandbox.enabled 1 first");
}

// A Sandbox request posted; later attempts wait for its status line (sandbox::SetStatus).
struct Pending {
    unsigned long long since = 0;
};

json::Value AwaitStatus(Pending& p, int attempt) {
    const sb::Status st = sb::GetStatus();
    if (st.tick >= p.since && st.tick != 0) {
        if (st.error) throw Error("Sandbox: " + st.text);
        json::Value r = Obj();
        r.set("status", Str(st.text));
        return r;
    }
    if (attempt > 30) throw Error("the Sandbox did not answer the request");
    return Retry();
}

// The Sandbox snapshot (item catalog), fresh from after `since`; nullptr = not yet (it is published while
// the Sandbox page is drawn: MarkViewed makes it think so).
std::shared_ptr<const sb::Snapshot> Snapshot(unsigned long long since) {
    sb::MarkViewed();
    auto s = sb::Get();
    return s && s->tick >= since && s->spawnables ? s : nullptr;
}

// ---- plots ----
struct PlotSyms {
    bool tried = false;
    Il2CppClass* plot = nullptr;
    Il2CppClass* location = nullptr;
    Il2CppClass* activator = nullptr;
    size_t typeId = 0, model = 0, locId = 0, actPlot = 0;
    const MethodInfo* hasUpgrade = nullptr;
    const MethodInfo* getParent = nullptr;
};
PlotSyms g_p;

void ResolvePlots() {
    if (g_p.tried) return;
    g_p.tried = true;
    auto& R = sr2::il2cpp::Resolver::Get();
    std::string err;
    g_p.plot = R.Class(kImage, "", "LandPlot", &err);
    g_p.location = R.Class(kImage, "", "LandPlotLocation", &err);
    g_p.activator = R.Class(kImage, "MonomiPark.SlimeRancher.UI", "LandPlotUIActivator", &err);
    g_p.typeId = game::FieldOffset(g_p.plot, "TypeId");
    g_p.model = game::FieldOffset(g_p.plot, "_model");
    g_p.locId = game::FieldOffset(g_p.location, "_id");
    g_p.actPlot = game::FieldOffset(g_p.activator, "landPlot");
    g_p.hasUpgrade = g_p.plot ? game::MethodOn(g_p.plot, "HasUpgrade", 1) : nullptr;
    g_p.getParent = game::MethodOn(R.Class("UnityEngine.CoreModule", "UnityEngine", "Transform", &err), "get_parent", 0);
}

const char* const kPlotTypes[] = {"none", "empty", "corral", "coop", "garden", "silo", "pond", "incinerator"};
const char* const kUpgrades[] = {"none",          "walls",          "music_box",     "storage2",      "storage3",
                                 "storage4",      "soil",           "sprinkler",     "scareslime",    "feeder",
                                 "vitamizer",     "air_net",        "plort_collector", "solar_shield", "ash_trough",
                                 "miracle_mix",   "deluxe_garden",  "deluxe_coop",   "plort_collector_pond",
                                 "plort_collector_incinerator", "storage_capacity_increase"};

std::string PlotId(Il2CppObject* plot) {
    Il2CppObject* t = unity::TransformOf(plot);
    Il2CppObject* parent = t && g_p.getParent ? Call(g_p.getParent, t, {}, "Transform.get_parent") : nullptr;
    Il2CppObject* loc = parent ? unity::GetComponent(parent, g_p.location) : nullptr;
    return loc && g_p.locId ? game::Utf8(unity::Field<Il2CppString*>(loc, g_p.locId)) : std::string();
}

// Live plots in the loaded scenes (FindObjectsOfTypeAll on request only, never per frame).
std::vector<Il2CppObject*> LivePlots() {
    ResolvePlots();
    if (!g_p.plot || !g_p.model) throw Error("LandPlot is unavailable");
    std::vector<Il2CppObject*> r;
    for (Il2CppObject* p : unity::FindObjectsOfTypeAll(g_p.plot)) {
        if (!unity::IsAlive(p) || !unity::Field<Il2CppObject*>(p, g_p.model)) continue;
        Il2CppObject* go = unity::GameObjectOf(p);
        if (go && unity::ActiveInHierarchy(go)) r.push_back(p);
    }
    return r;
}

json::Value PlotsProbe(const json::Value&) {
    if (!game::InGame()) throw Error("not in game");
    json::Value list = Arr();
    for (Il2CppObject* p : LivePlots()) {
        json::Value o = Obj();
        o.set("id", Str(PlotId(p)));
        const int type = g_p.typeId ? unity::Field<int>(p, g_p.typeId) : 0;
        o.set("type", Str(type >= 0 && type < 8 ? kPlotTypes[type] : std::to_string(type)));
        json::Value ups = Arr();
        if (g_p.hasUpgrade)
            for (int u = 1; u <= 20; ++u)
                if (CallValue<bool>(g_p.hasUpgrade, p, {&u}, "LandPlot.HasUpgrade")) ups.push(Str(kUpgrades[u]));
        json::Value custom = Arr();
        if (cu::Active())
            for (int32_t id = cu::kIdBase + 1; id < cu::kIdBase + 0x80; ++id)
                if (cu::Owns(p, id)) custom.push(Str(cu::Name(id)));
        o.set("upgrades", std::move(ups));
        o.set("custom_upgrades", std::move(custom));
        game::Vec3f pos;
        if (game::Position(p, pos)) o.set("pos", Vec3(pos.x, pos.y, pos.z));
        list.push(std::move(o));
    }
    json::Value r = Obj();
    r.set("plots", std::move(list));
    return r;
}

json::Value PlotMenu(const json::Value& args) {
    const std::string id = args["plot"].asString();
    if (Attempt(args) > 0) {  // opened last time: is the menu up?
        for (const std::string& m : game::OpenMenus())
            if (m == "LandPlotUIRoot") {
                json::Value r = Obj();
                r.set("opened", Bool(true));
                return r;
            }
        if (Attempt(args) > 20) throw Error("the plot menu did not open");
        return Retry();
    }
    if (!game::InGame()) throw Error("not in game");
    if (id.empty()) throw Error("plot_menu needs \"plot\" (an id from the plots probe)");
    Il2CppObject* plot = nullptr;
    for (Il2CppObject* p : LivePlots())
        if (PlotId(p) == id) plot = p;
    if (!plot) throw Error("no loaded plot with id " + id);
    if (!g_p.activator || !g_p.actPlot) throw Error("LandPlotUIActivator is unavailable");
    for (Il2CppObject* a : unity::FindObjectsOfTypeAll(g_p.activator)) {
        if (!unity::IsAlive(a) || unity::Field<Il2CppObject*>(a, g_p.actPlot) != plot) continue;
        // runtime_invoke does no virtual dispatch: the method from the object's own class up.
        const MethodInfo* m = game::MethodOn(sr2::il2cpp::api().il2cpp_object_get_class(a), "OnInteract", 0);
        Call(m, a, {}, "LandPlotUIActivator.OnInteract");
        SR2_INFO("[{}] plot_menu: opened the menu of plot {}", kModule, id);
        return Retry();  // check on the next attempt
    }
    throw Error("no menu activator found for plot " + id);
}

// ---- time ----
json::Value SetTime(const json::Value& args) {
    double hour = 0, now = 0;
    int day = 0;
    if (!game::Clock(hour, day, now)) throw Error("not in game");
    double target = now;
    if (const json::Value* h = args.find("hour"); h && h->isNumber()) {
        double want = std::fmod(h->asNumber(), 24.0);
        if (want < 0) want += 24;
        double delta = want - hour;
        if (delta <= 0.001) delta += 24;  // the next time the clock shows that hour
        target = now + delta * 3600.0;
    }
    if (const json::Value* a = args.find("add_hours"); a && a->isNumber()) {
        if (a->asNumber() < 0) throw Error("add_hours must be >= 0 (the clock only runs forward)");
        target += a->asNumber() * 3600.0;
    }
    if (target <= now) throw Error("set_time needs \"hour\" (0-24) or \"add_hours\"");
    game::FastForwardTo(target);
    SR2_INFO("[{}] set_time: fast-forward from day {} {:.2f} h to world time {:.0f} (+{:.2f} h)", kModule, day, hour, target,
             (target - now) / 3600.0);
    json::Value r = Obj();
    r.set("from_hour", Num(hour));
    r.set("from_day", Num(day));
    r.set("until_world_time", Num(target));
    r.set("hours", Num((target - now) / 3600.0));
    r.set("note", Str("fast-forward (as sleeping does): the world catches up over a few seconds"));
    return r;
}

// ---- Sandbox ----
json::Value GiveNewbucks(const json::Value& args) {
    static Pending p;
    if (Attempt(args) > 0) {
        json::Value r = AwaitStatus(p, Attempt(args));
        int nb = 0;
        if (!r.find("retry") && game::Newbucks(nb)) r.set("newbucks", Num(nb));
        return r;
    }
    NeedSandbox();
    if (!game::InGame()) throw Error("not in game");
    sb::Action a;
    a.op = sb::Op::AddNewbucks;
    a.value = args["amount"].asInt(0);
    if (a.value == 0) throw Error("give_newbucks needs a non-zero \"amount\"");
    p.since = GetTickCount64();
    if (!sb::Request(a)) throw Error("the main-thread dispatcher is not running");
    return Retry();
}

json::Value Spawn(const json::Value& args) {
    static Pending p;
    static uintptr_t item = 0;
    static unsigned long long viewSince = 0;
    const int attempt = Attempt(args);
    if (attempt == 0) {
        NeedSandbox();
        if (!game::InGame()) throw Error("not in game");
        if (args["id"].asString().empty()) throw Error("spawn needs \"id\" (an item name as the Sandbox spawner lists it)");
        viewSince = GetTickCount64();
        item = 0;
        p.since = 0;
    }
    if (p.since) return AwaitStatus(p, attempt);
    auto snap = Snapshot(viewSince);
    if (!snap) {
        if (attempt > kSnapshotWaitTries) throw Error("the Sandbox item list did not come (is the Sandbox on and a save loaded?)");
        return Retry();
    }
    if (!snap->spawnWhy.empty()) throw Error("the Sandbox spawner is unavailable: " + snap->spawnWhy);
    const std::string want = Norm(args["id"].asString());
    std::vector<const sb::Item*> partial;
    for (const sb::Item& it : *snap->spawnables) {
        const std::string n = Norm(it.name);
        if (n == want) {
            item = it.id;
            break;
        }
        if (n.find(want) != std::string::npos) partial.push_back(&it);
    }
    if (!item && partial.size() == 1) item = partial[0]->id;
    if (!item) {
        std::string some;
        for (size_t i = 0; i < partial.size() && i < 8; ++i) some += (i ? ", " : "") + partial[i]->name;
        throw Error(partial.empty() ? "unknown item '" + args["id"].asString() + "' (not in the Sandbox spawn list)"
                                    : "ambiguous item '" + args["id"].asString() + "': " + some);
    }
    sb::Action a;
    a.op = sb::Op::Spawn;
    a.item = item;
    a.value = std::clamp<int64_t>(args["count"].asInt(1), 1, 60);
    p.since = GetTickCount64();
    if (!sb::Request(a)) throw Error("the main-thread dispatcher is not running");
    return Retry();
}

json::Value UpgradeLevel(const json::Value& args) {
    static Pending p;
    static unsigned long long viewSince = 0;
    const int attempt = Attempt(args);
    if (attempt == 0) {
        NeedSandbox();
        if (!game::InGame()) throw Error("not in game");
        if (args["id"].asString().empty() || !args["level"].isNumber())
            throw Error("sandbox.upgrade_level needs \"id\" (an upgrade name, e.g. tank_booster) and \"level\" (-1 = not bought, 0 = tier I, ...)");
        viewSince = GetTickCount64();
        p.since = 0;
    }
    if (p.since) return AwaitStatus(p, attempt);
    sb::MarkViewed();  // the upgrades state is published while the page is open
    auto st = ub::Get();
    if (!st || st->tick < viewSince) {
        if (attempt > kSnapshotWaitTries) throw Error("the Sandbox upgrade list did not come");
        return Retry();
    }
    if (!st->inGame || st->upgrades.empty()) throw Error("the Sandbox upgrade list is empty: " + st->why);
    const std::string want = Norm(args["id"].asString());
    const ub::Upgrade* up = nullptr;
    for (const ub::Upgrade& u : st->upgrades)
        if (Norm(u.name) == want) up = &u;
    if (!up) {
        std::string names;
        for (const ub::Upgrade& u : st->upgrades) names += (names.empty() ? "" : ", ") + u.name;
        throw Error("unknown upgrade '" + args["id"].asString() + "' (known: " + names + ")");
    }
    const int level = static_cast<int>(args["level"].asInt());
    if (level < -1 || level >= up->levels)
        throw Error(std::format("level {} out of range for {} (-1 .. {})", level, up->name, up->levels - 1));
    ub::Action a;
    a.op = ub::Op::SetLevel;
    a.id = up->id;
    a.value = level;
    p.since = GetTickCount64();
    if (!ub::Request(a)) throw Error("the main-thread dispatcher is not running");
    return Retry();
}

json::Value SandboxStatus(const json::Value&) {
    const sb::Status st = sb::GetStatus();
    json::Value r = Obj();
    r.set("text", Str(st.text));
    r.set("error", Bool(st.error));
    r.set("age_ms", Num(st.tick ? static_cast<double>(GetTickCount64() - st.tick) : -1.0));
    return r;
}

}  // namespace

void RegisterGenericActions() {
    RegisterProbe("plots", PlotsProbe);
    RegisterAction("plot_menu", PlotMenu);
    RegisterAction("set_time", SetTime);
    RegisterAction("give_newbucks", GiveNewbucks);
    RegisterAction("spawn", Spawn);
    RegisterAction("sandbox.upgrade_level", UpgradeLevel);
    RegisterProbe("sandbox.status", SandboxStatus);
}

}  // namespace sr2::modules::autotest
