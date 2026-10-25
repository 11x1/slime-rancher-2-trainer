#pragma once
// autotest: the probe / action registry of the test harness (see NOTES.md).
//
// A module registers what its tests need to read (probes) and to do (actions) by name, e.g.
// "feeder.lock" or "slime_dj.lamps"; the harness calls them for the pipe commands
// {"cmd":"probe","args":{"name":...,"args":{...}}} and {"cmd":"exec",...}.
//
//   #include "modules/autotest/api.h"
//   namespace at = sr2::modules::autotest;
//   at::RegisterProbe("slime_dj.lamps", [](const sr2::json::Value& args) {
//       sr2::json::Value r = at::Obj();
//       r.set("count", at::Num(3));
//       return r;
//   });
//
// Rules:
//   * Registration is cheap (a map insert under a mutex), any thread, any time (Setup is a good place);
//     re-registering a name replaces it. Nothing is called while the harness is off.
//   * The functions run on the Unity main thread, one at a time, never inside a hook.
//   * Throw autotest::Error("text") for a failure: the client gets {"ok":false,"error":"text"}.
//   * Something that needs a few frames (a teleport, a menu opening): return Retry(). The harness calls
//     the function again ~100 ms later with args "_attempt" = 1, 2, ... until it returns anything else or
//     the command's timeout runs out. Attempt(args) reads the counter (0 on the first call).
//   * Never load, save or delete a game from a probe or action: the save-slot rules live in the
//     harness's own commands (load_slot, save, ...).

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/json.h"

namespace sr2::modules::autotest {

using Fn = std::function<json::Value(const json::Value& args)>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void RegisterProbe(const std::string& name, Fn fn);
void RegisterAction(const std::string& name, Fn fn);

// "Call me again" (see above) and the retry counter of this call.
json::Value Retry();
int Attempt(const json::Value& args);

// JSON helpers (json::Value's own constructors are explicit; a string literal would pick bool).
json::Value Obj();
json::Value Arr();
json::Value Str(std::string s);
json::Value Num(double d);
json::Value Bool(bool b);
json::Value Vec3(float x, float y, float z);

// ---- harness side ----
bool FindFn(bool probe, const std::string& name, Fn& out);
std::vector<std::string> Names(bool probes);

}  // namespace sr2::modules::autotest
