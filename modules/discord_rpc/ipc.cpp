// discord_rpc: Discord IPC over the local named pipe, on one background thread
// (see ipc.h and NOTES.md).

#include "modules/discord_rpc/ipc.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include "core/json.h"
#include "core/log.h"
#include "core/settings.h"
#include "ui/discord_bridge.h"

namespace sr2::modules::discord_rpc {
namespace {

namespace bridge = sr2::ui::discord;
namespace settings = sr2::core::settings;

constexpr const char* kModule = "discord_rpc";
constexpr DWORD kTickMs = 1000;                         // worker wake-up without news
constexpr unsigned long long kMinSendMs = 5000;         // Discord allows 5 SET_ACTIVITY per 20 s
constexpr unsigned long long kRetryMinMs = 5000;        // Discord not running: first retry
constexpr unsigned long long kRetryMaxMs = 30000;       //   ... backing off to this
constexpr unsigned long long kRejectedRetryMs = 120000; // application id refused (unless it changes)
constexpr DWORD kHandshakeMs = 3000;
constexpr uint32_t kMaxFrame = 64 * 1024;
constexpr size_t kMaxField = 128;                       // Discord's limit for details / state

enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

// ---- shared with the publishing threads (g_mutex) ----
std::mutex g_mutex;
Presence g_want;
bool g_haveWant = false;
bool g_active = false;
unsigned long long g_version = 0;
HANDLE g_wake = nullptr;  // auto-reset; set on every change
bool g_started = false;

// ---- worker only ----
HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::string g_connectedId;
unsigned long long g_nonce = 0;
std::string g_lastStatus;     // what was last logged / shown
std::string g_lastError;      // last ERROR reply logged (one line per distinct message)

void Status(const std::string& text, bridge::Level level, bool log) {
    bridge::SetStatus(text, level);
    if (text == g_lastStatus) return;
    g_lastStatus = text;
    if (!log) return;
    if (level == bridge::Level::Error || level == bridge::Level::Warn) SR2_WARN("[{}] {}", kModule, text);
    else SR2_INFO("[{}] {}", kModule, text);
}

std::string Trim(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool ValidId(const std::string& id) {
    return id.size() >= 15 && id.size() <= 21 && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// At most kMaxField bytes, cut at a UTF-8 character boundary.
std::string Clip(std::string s) {
    if (s.size() <= kMaxField) return s;
    size_t n = kMaxField;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

bool Connected() { return g_pipe != INVALID_HANDLE_VALUE; }

void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    g_connectedId.clear();
}

bool WriteAll(const void* data, DWORD size) {
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD done = 0;
        if (!WriteFile(g_pipe, p, size, &done, nullptr) || done == 0) return false;
        p += done;
        size -= done;
    }
    return true;
}

bool WriteFrame(uint32_t op, const std::string& json) {
    if (!Connected() || json.size() > kMaxFrame) return false;
    uint32_t header[2] = {op, static_cast<uint32_t>(json.size())};
    std::string buf(reinterpret_cast<const char*>(header), sizeof(header));
    buf += json;
    return WriteAll(buf.data(), static_cast<DWORD>(buf.size()));
}

bool ReadAll(void* data, DWORD size) {
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        DWORD done = 0;
        if (!ReadFile(g_pipe, p, size, &done, nullptr) || done == 0) return false;
        p += done;
        size -= done;
    }
    return true;
}

// 1: a frame was read; 0: nothing within waitMs; -1: the pipe is broken or the frame is bad.
// Only reads once a whole header is waiting, so it never blocks for long.
int ReadFrame(uint32_t& op, std::string& json, DWORD waitMs) {
    const unsigned long long until = GetTickCount64() + waitMs;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (avail >= 8) break;
        if (GetTickCount64() >= until) return 0;
        Sleep(20);
    }
    uint32_t header[2] = {};
    if (!ReadAll(header, sizeof(header))) return -1;
    if (header[1] > kMaxFrame) return -1;
    op = header[0];
    json.assign(header[1], '\0');
    return header[1] == 0 || ReadAll(json.data(), header[1]) ? 1 : -1;
}

std::string Compact(const json::Value& v) {
    // json::Serialize pretty-prints; newlines and indentation are valid JSON, so this is only cosmetic.
    return json::Serialize(v);
}

enum class ConnectResult { Ok, NoDiscord, Rejected, Failed };

ConnectResult Connect(const std::string& id, std::string& detail) {
    for (int i = 0; i < 10 && !Connected(); ++i) {
        const std::wstring name = std::format(L"\\\\.\\pipe\\discord-ipc-{}", i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) g_pipe = h;
    }
    if (!Connected()) return ConnectResult::NoDiscord;
    json::Value hs = json::Value::Object();
    hs.set("v", json::Value(1.0));
    hs.set("client_id", json::Value(id));
    uint32_t op = 0;
    std::string reply;
    if (!WriteFrame(kHandshake, Compact(hs)) || ReadFrame(op, reply, kHandshakeMs) != 1) {
        Disconnect();
        detail = "no reply to the handshake";
        return ConnectResult::Failed;
    }
    json::Value doc;
    std::string perr;
    json::Parse(reply, doc, perr);
    if (op == kClose) {
        Disconnect();
        detail = doc["message"].asString("closed");
        return ConnectResult::Rejected;
    }
    if (op != kFrame || doc["evt"].asString() != "READY") {
        Disconnect();
        detail = std::format("unexpected reply (op {}, {})", op, doc["evt"].asString("no event"));
        return ConnectResult::Failed;
    }
    const json::Value& user = doc["data"]["user"];
    detail = user["global_name"].asString();
    if (detail.empty()) detail = user["username"].asString();
    g_connectedId = id;
    g_lastError.clear();
    return ConnectResult::Ok;
}

// Activity (nullptr = clear it). Discord answers with a FRAME, read by Poll later.
bool SendActivity(const Presence* p) {
    json::Value args = json::Value::Object();
    args.set("pid", json::Value(static_cast<double>(GetCurrentProcessId())));
    if (p) {
        json::Value act = json::Value::Object();
        const std::string details = Clip(p->details), state = Clip(p->state);
        if (details.size() >= 2) act.set("details", json::Value(details));  // Discord wants 2..128 characters
        if (state.size() >= 2) act.set("state", json::Value(state));
        if (p->start > 0) {
            json::Value ts = json::Value::Object();
            ts.set("start", json::Value(static_cast<double>(p->start)));
            act.set("timestamps", std::move(ts));
        }
        args.set("activity", std::move(act));
    } else {
        args.set("activity", json::Value());  // null clears the presence
    }
    json::Value cmd = json::Value::Object();
    cmd.set("cmd", json::Value(std::string("SET_ACTIVITY")));
    cmd.set("args", std::move(args));
    cmd.set("nonce", json::Value(std::to_string(++g_nonce)));
    return WriteFrame(kFrame, Compact(cmd));
}

// Answers pings and reads replies. False when the pipe is gone (Discord quit).
bool Poll() {
    uint32_t op = 0;
    std::string payload;
    for (int i = 0; i < 16; ++i) {
        const int r = ReadFrame(op, payload, 0);
        if (r == 0) return true;
        if (r < 0) return false;
        if (op == kPing) {
            if (!WriteFrame(kPong, payload)) return false;
        } else if (op == kClose) {
            return false;
        } else if (op == kFrame) {
            json::Value doc;
            std::string perr;
            if (json::Parse(payload, doc, perr) && doc["evt"].asString() == "ERROR") {
                const std::string msg = doc["data"]["message"].asString("unknown error");
                if (msg != g_lastError) {
                    g_lastError = msg;
                    SR2_WARN("[{}] Discord refused the presence: {}", kModule, msg);
                }
            }
        }
    }
    return true;
}

DWORD WINAPI Worker(LPVOID) {
    unsigned long long nextTry = 0, lastSend = 0, backoff = kRetryMinMs, sentVersion = 0;
    std::string rejectedId;
    for (;;) {
        WaitForSingleObject(g_wake, kTickMs);
        bool active = false, have = false;
        Presence want;
        unsigned long long version = 0;
        {
            std::lock_guard lock(g_mutex);
            active = g_active;
            have = g_haveWant;
            want = g_want;
            version = g_version;
        }
        const std::string id = Trim(settings::GetString(bridge::kModule, bridge::kKeyAppId));
        const unsigned long long now = GetTickCount64();
        auto drop = [&] {
            if (Connected()) {
                SendActivity(nullptr);
                Disconnect();
                SR2_INFO("[{}] disconnected from Discord", kModule);
            }
        };
        if (!active) {
            drop();
            Status("Off.", bridge::Level::Dim, false);
            nextTry = 0, backoff = kRetryMinMs;
            continue;
        }
        if (id.empty() || !ValidId(id)) {
            drop();
            Status(id.empty() ? "No application id set. Paste your Discord application id below."
                              : "The application id is not valid: it is the number (about 18-20 digits) from the "
                                "Discord developer portal.",
                   bridge::Level::Warn, true);
            continue;
        }
        if (Connected() && id != g_connectedId) {
            drop();
            nextTry = 0, backoff = kRetryMinMs;
        }
        if (!Connected()) {
            if (!rejectedId.empty() && id != rejectedId) {  // a new id after a refusal: try it at once
                rejectedId.clear();
                nextTry = 0;
            }
            if (now < nextTry) continue;
            std::string detail;
            switch (Connect(id, detail)) {
                case ConnectResult::Ok:
                    rejectedId.clear();
                    backoff = kRetryMinMs;
                    sentVersion = 0;  // send the current presence again
                    Status(detail.empty() ? "Connected to Discord." : std::format("Connected to Discord as {}.", detail),
                           bridge::Level::Ok, true);
                    break;
                case ConnectResult::NoDiscord:
                    nextTry = now + backoff;
                    backoff = std::min(backoff * 2, kRetryMaxMs);
                    Status("Waiting for Discord (the desktop app is not running).", bridge::Level::Dim, true);
                    continue;
                case ConnectResult::Rejected:
                    rejectedId = id;
                    nextTry = now + kRejectedRetryMs;
                    Status(std::format("Discord refused the application id: {}", detail), bridge::Level::Error, true);
                    continue;
                case ConnectResult::Failed:
                    nextTry = now + backoff;
                    backoff = std::min(backoff * 2, kRetryMaxMs);
                    Status(std::format("Could not talk to Discord ({}); retrying.", detail), bridge::Level::Warn, true);
                    continue;
            }
        }
        if (!Poll()) {
            Disconnect();
            nextTry = now + kRetryMinMs;
            Status("Discord closed the connection; waiting for it.", bridge::Level::Dim, true);
            continue;
        }
        if (have && version != sentVersion && now - lastSend >= kMinSendMs) {
            if (SendActivity(&want)) {
                sentVersion = version;
                lastSend = now;
                SR2_DEBUG("[{}] presence: {} | {}", kModule, want.details, want.state);
            } else {
                Disconnect();
                nextTry = now + kRetryMinMs;
                Status("Discord closed the connection; waiting for it.", bridge::Level::Dim, true);
            }
        }
    }
}

void StartLocked() {
    if (g_started) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) {
        SR2_ERROR("[{}] could not create the worker event ({})", kModule, GetLastError());
        return;
    }
    // Lives for the rest of the process (waits on the event while off); never joined.
    if (HANDLE t = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) {
        CloseHandle(t);
        g_started = true;
    } else {
        SR2_ERROR("[{}] could not start the pipe thread ({})", kModule, GetLastError());
    }
}

}  // namespace

void SetActive(bool on) {
    std::lock_guard lock(g_mutex);
    if (on) StartLocked();
    g_active = on;
    if (!on) g_haveWant = false;
    if (g_wake) SetEvent(g_wake);
}

void Publish(const Presence& p) {
    std::lock_guard lock(g_mutex);
    if (g_haveWant && p == g_want) return;
    g_want = p;
    g_haveWant = true;
    ++g_version;
    if (g_wake) SetEvent(g_wake);
}

}  // namespace sr2::modules::discord_rpc
