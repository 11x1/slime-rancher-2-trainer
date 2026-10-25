// autotest: probe / action registry (api.h).

#include <map>
#include <mutex>

#include "modules/autotest/api.h"

namespace sr2::modules::autotest {
namespace {

std::mutex g_mutex;
std::map<std::string, Fn> g_probes, g_actions;  // guarded by g_mutex

}  // namespace

void RegisterProbe(const std::string& name, Fn fn) {
    std::lock_guard lk(g_mutex);
    g_probes[name] = std::move(fn);
}

void RegisterAction(const std::string& name, Fn fn) {
    std::lock_guard lk(g_mutex);
    g_actions[name] = std::move(fn);
}

bool FindFn(bool probe, const std::string& name, Fn& out) {
    std::lock_guard lk(g_mutex);
    const auto& m = probe ? g_probes : g_actions;
    auto it = m.find(name);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

std::vector<std::string> Names(bool probes) {
    std::lock_guard lk(g_mutex);
    std::vector<std::string> r;
    for (const auto& [k, v] : probes ? g_probes : g_actions) r.push_back(k);
    return r;
}

json::Value Retry() {
    json::Value v = json::Value::Object();
    v.set("retry", json::Value(true));
    return v;
}

int Attempt(const json::Value& args) { return static_cast<int>(args["_attempt"].asInt(0)); }

json::Value Obj() { return json::Value::Object(); }
json::Value Arr() { return json::Value::Array(); }
json::Value Str(std::string s) { return json::Value(std::move(s)); }
json::Value Num(double d) { return json::Value(d); }
json::Value Bool(bool b) { return json::Value(b); }
json::Value Vec3(float x, float y, float z) {
    json::Value a = json::Value::Array();
    a.push(json::Value(static_cast<double>(x)));
    a.push(json::Value(static_cast<double>(y)));
    a.push(json::Value(static_cast<double>(z)));
    return a;
}

}  // namespace sr2::modules::autotest
