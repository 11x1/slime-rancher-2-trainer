// autotest: named pipe transport (pipe.h).

#include "modules/autotest/pipe.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/paths.h"
#include "modules/autotest/api.h"

namespace sr2::modules::autotest::pipe {
namespace {

constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\sr2mods-autotest";
constexpr DWORD kBufSize = 64 * 1024;
constexpr size_t kMaxLine = 1024 * 1024;
constexpr DWORD kTickMs = 100;          // server loop wake-up (log tail, stop flag)
constexpr DWORD kWriteTimeoutMs = 5000;
constexpr size_t kMaxQueued = 64;

struct Request {
    json::Value id;  // echoed as is (number or string); Null when the request had none
    std::string cmd;
    json::Value args;
    unsigned generation = 0;
};

Handler g_handler;
QuickHandler g_quick;
ConnectHandler g_onConnect;

std::atomic<bool> g_stop{false};
std::atomic<int> g_threads{0};
HANDLE g_stopEvent = nullptr;  // manual reset, set by Stop

std::mutex g_writeMutex;           // the pipe handle and writes
HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::atomic<bool> g_connected{false};
std::atomic<unsigned> g_generation{0};  // +1 per client
std::atomic<bool> g_cancel{false};      // the running command's cancel flag (client gone / stop)

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<Request> g_queue;
std::atomic<bool> g_busy{false};  // the command thread runs a request

std::atomic<bool> g_logWanted{false};

// Writes one line (adds '\n'). False when nobody is connected or the write failed.
bool WriteLine(const std::string& text, unsigned generation) {
    std::lock_guard lk(g_writeMutex);
    if (g_pipe == INVALID_HANDLE_VALUE || !g_connected.load() || (generation && generation != g_generation.load()))
        return false;
    std::string data = text;
    data += '\n';
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    bool ok = WriteFile(g_pipe, data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, kWriteTimeoutMs) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(g_pipe, &ov, &done, FALSE) != FALSE;
        } else {
            CancelIoEx(g_pipe, &ov);
            GetOverlappedResult(g_pipe, &ov, &done, TRUE);
            ok = false;
        }
    } else if (ok) {
        GetOverlappedResult(g_pipe, &ov, &done, TRUE);
    }
    CloseHandle(ov.hEvent);
    return ok && done == data.size();
}

json::Value Reply(const json::Value& id, bool ok, json::Value payload) {
    json::Value r = json::Value::Object();
    r.set("id", id);
    r.set("ok", json::Value(ok));
    if (ok) r.set("result", std::move(payload));
    else r.set("error", std::move(payload));
    return r;
}

void Answer(const Request& rq, bool ok, json::Value payload) {
    WriteLine(json::SerializeCompact(Reply(rq.id, ok, std::move(payload))), rq.generation);
}

// ------------------------------------------------------------ command thread
void CommandThread() {
    for (;;) {
        Request rq;
        {
            std::unique_lock lk(g_queueMutex);
            g_queueCv.wait_for(lk, std::chrono::milliseconds(kTickMs), [] { return !g_queue.empty() || g_stop.load(); });
            if (g_stop.load()) break;
            if (g_queue.empty()) continue;
            rq = std::move(g_queue.front());
            g_queue.pop_front();
            g_busy = true;
        }
        if (rq.generation != g_generation.load()) {  // its client is gone
            g_busy = false;
            continue;
        }
        try {
            json::Value result = g_handler(rq.cmd, rq.args, g_cancel);
            Answer(rq, true, std::move(result));
        } catch (const Error& e) {
            Answer(rq, false, json::Value(std::string(e.what())));
        } catch (const std::exception& e) {
            Answer(rq, false, json::Value(std::string("internal error: ") + e.what()));
        } catch (...) {
            Answer(rq, false, json::Value(std::string("internal error")));
        }
        g_busy = false;
    }
    g_threads.fetch_sub(1);
}

// ------------------------------------------------------------- log tail -----
struct LogTail {
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned long long offset = 0;
    std::string partial;
    bool on = false;

    void Close() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        partial.clear();
        on = false;
    }
    void Tick(unsigned generation) {
        const bool want = g_logWanted.load();
        if (!want) {
            if (on) Close();
            return;
        }
        if (!on) {
            const std::wstring path = (sr2::paths::SelfDir() / L"sr2mods.log").wstring();
            file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            LARGE_INTEGER size{};
            GetFileSizeEx(file, &size);
            offset = static_cast<unsigned long long>(size.QuadPart);  // only lines from now on
            on = true;
            return;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) return;
        if (static_cast<unsigned long long>(size.QuadPart) < offset) offset = 0;  // truncated
        int lines = 0;
        while (static_cast<unsigned long long>(size.QuadPart) > offset && lines < 500) {
            char buf[16384];
            LARGE_INTEGER at{};
            at.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
            DWORD got = 0;
            if (!ReadFile(file, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            offset += got;
            partial.append(buf, got);
            size_t nl;
            while ((nl = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, nl);
                partial.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                json::Value ev = json::Value::Object();
                ev.set("event", json::Value(std::string("log")));
                ev.set("line", json::Value(std::move(line)));
                WriteLine(json::SerializeCompact(ev), generation);
                ++lines;
            }
            if (partial.size() > kMaxLine) partial.clear();
        }
    }
};

// -------------------------------------------------------------- server ------
void HandleLine(const std::string& line, unsigned generation) {
    json::Value msg;
    std::string err;
    Request rq;
    rq.generation = generation;
    if (!json::Parse(line, msg, err) || !msg.isObject()) {
        Answer(rq, false, json::Value("bad request (not a JSON object): " + err));
        return;
    }
    rq.id = msg["id"];
    rq.cmd = msg["cmd"].asString();
    rq.args = msg["args"].isObject() ? msg["args"] : json::Value::Object();
    if (rq.cmd.empty()) {
        Answer(rq, false, json::Value(std::string("unknown command: missing \"cmd\"")));
        return;
    }
    try {
        json::Value result;
        if (g_quick && g_quick(rq.cmd, rq.args, result)) {
            Answer(rq, true, std::move(result));
            return;
        }
    } catch (const Error& e) {
        Answer(rq, false, json::Value(std::string(e.what())));
        return;
    } catch (const std::exception& e) {
        Answer(rq, false, json::Value(std::string("internal error: ") + e.what()));
        return;
    }
    std::lock_guard lk(g_queueMutex);
    if (g_queue.size() >= kMaxQueued) {
        Answer(rq, false, json::Value(std::string("busy: too many queued requests")));
        return;
    }
    g_queue.push_back(std::move(rq));
    g_queueCv.notify_all();
}

// One client session on a connected pipe. Returns when it leaves or Stop is called.
void Session(HANDLE pipe, unsigned generation) {
    HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readEvent) return;
    std::string pending;
    LogTail tail;
    char buf[kBufSize];
    bool reading = false;
    OVERLAPPED ov{};
    for (;;) {
        if (g_stop.load()) break;
        if (!reading) {
            ov = OVERLAPPED{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD got = 0;
            if (ReadFile(pipe, buf, sizeof(buf), &got, &ov)) {
                // finished at once: handled below through GetOverlappedResult
            } else if (GetLastError() != ERROR_IO_PENDING) {
                break;  // client gone
            }
            reading = true;
        }
        HANDLE waits[2] = {readEvent, g_stopEvent};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kTickMs);
        if (w == WAIT_OBJECT_0) {
            DWORD got = 0;
            reading = false;
            if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) {
                if (GetLastError() == ERROR_MORE_DATA) {
                    pending.append(buf, got);
                    continue;
                }
                break;  // broken pipe: the client left
            }
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) HandleLine(line, generation);
            }
            if (pending.size() > kMaxLine) {
                SR2_WARN("[autotest] request line over {} bytes: dropped", kMaxLine);
                pending.clear();
            }
        } else if (w == WAIT_OBJECT_0 + 1) {
            break;
        }
        tail.Tick(generation);
    }
    if (reading) {
        CancelIoEx(pipe, &ov);
        DWORD got = 0;
        GetOverlappedResult(pipe, &ov, &got, TRUE);
    }
    tail.Close();
    CloseHandle(readEvent);
}

void ServerThread() {
    bool loggedFail = false;
    while (!g_stop.load()) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBufSize,
                                       kBufSize, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (!loggedFail) {
                loggedFail = true;
                SR2_WARN("[autotest] cannot create the pipe (error {}); retrying every second", GetLastError());
            }
            WaitForSingleObject(g_stopEvent, 1000);
            continue;
        }
        if (loggedFail) SR2_INFO("[autotest] pipe created after all");
        loggedFail = false;
        HANDLE connEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.hEvent = connEvent;
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (e == ERROR_IO_PENDING) {
                HANDLE waits[2] = {connEvent, g_stopEvent};
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                DWORD dummy = 0;
                if (w == WAIT_OBJECT_0) {
                    connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe, &ov);
                    GetOverlappedResult(pipe, &ov, &dummy, TRUE);
                }
            }
        }
        if (connEvent) CloseHandle(connEvent);
        if (!connected || g_stop.load()) {
            CloseHandle(pipe);
            continue;
        }
        const unsigned gen = g_generation.fetch_add(1) + 1;
        g_cancel = false;
        {
            std::lock_guard lk(g_writeMutex);
            g_pipe = pipe;
            g_connected = true;
        }
        SR2_INFO("[autotest] client connected (session {})", gen);
        if (g_onConnect) g_onConnect(true);
        Session(pipe, gen);
        // Gone: cancel the running command and drop the queue before anyone may write again.
        g_cancel = true;
        {
            std::lock_guard lk(g_queueMutex);
            g_queue.clear();
        }
        g_logWanted = false;
        for (int i = 0; i < 100 && g_busy.load(); ++i) Sleep(20);  // the command sees g_cancel
        {
            std::lock_guard lk(g_writeMutex);
            g_connected = false;
            g_pipe = INVALID_HANDLE_VALUE;
        }
        g_generation.fetch_add(1);  // late replies of this session are dropped
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        SR2_INFO("[autotest] client disconnected (session {})", gen);
        if (g_onConnect) g_onConnect(false);
    }
    g_threads.fetch_sub(1);
}

}  // namespace

void Start(Handler handler, QuickHandler quick, ConnectHandler onConnect) {
    // A previous Stop: its threads end within ~100 ms (a command waiting on the main thread checks the
    // cancel flag every 50 ms).
    for (int i = 0; i < 100 && g_threads.load() > 0; ++i) Sleep(20);
    if (g_threads.load() > 0) {
        SR2_ERROR("[autotest] the previous pipe threads did not end; the harness stays off");
        return;
    }
    g_handler = std::move(handler);
    g_quick = std::move(quick);
    g_onConnect = std::move(onConnect);
    if (!g_stopEvent) g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_stopEvent);
    g_stop = false;
    g_cancel = false;
    g_threads = 2;
    std::thread(ServerThread).detach();
    std::thread(CommandThread).detach();
    SR2_INFO("[autotest] pipe server started (\\\\.\\pipe\\sr2mods-autotest, local clients only)");
}

void Stop() {
    if (g_threads.load() == 0) return;
    g_stop = true;
    g_cancel = true;
    if (g_stopEvent) SetEvent(g_stopEvent);
    g_queueCv.notify_all();
    SR2_INFO("[autotest] pipe server stopping");
}

bool Connected() { return g_connected.load(); }

void SendEvent(const json::Value& ev) { WriteLine(json::SerializeCompact(ev), 0); }

void SubscribeLog(bool on) { g_logWanted = on; }

}  // namespace sr2::modules::autotest::pipe
