#pragma once
// autotest: the named pipe \\.\pipe\sr2mods-autotest (one client, line-delimited JSON). Two threads of
// its own, neither of which ever touches Unity:
//   * the server thread creates the pipe (PIPE_REJECT_REMOTE_CLIENTS, default security = the current
//     user), waits for a client, reads lines, answers the quick commands itself and queues the others;
//     it also tails sr2mods.log for subscribe_log;
//   * the command thread runs the queued requests one at a time through the Handler (which hands game
//     work to the main thread and waits) and writes each reply.
// See NOTES.md for the protocol.

#include <atomic>
#include <functional>
#include <string>

#include "core/json.h"

namespace sr2::modules::autotest::pipe {

// Runs one queued request (command thread). Returns the result or throws autotest::Error.
// `cancel` turns true when the client goes away or the harness stops.
using Handler = std::function<json::Value(const std::string& cmd, const json::Value& args, const std::atomic<bool>& cancel)>;
// Server thread: a command answered at once, not queued behind a running one (cancel_ask, ping, ...).
// true = handled (`result` set, or throws autotest::Error).
using QuickHandler = std::function<bool(const std::string& cmd, const json::Value& args, json::Value& result)>;
// A client connected (true) / left (false). Server thread.
using ConnectHandler = std::function<void(bool connected)>;

void Start(Handler handler, QuickHandler quick, ConnectHandler onConnect);
void Stop();  // returns at once; the threads end within ~100 ms (a running command is cancelled)
bool Connected();
// Pushes {"event": ...} to the client (any thread; dropped when nobody is connected).
void SendEvent(const json::Value& ev);
// subscribe_log: from now on every new sr2mods.log line goes out as {"event":"log","line":...}.
void SubscribeLog(bool on);

}  // namespace sr2::modules::autotest::pipe
