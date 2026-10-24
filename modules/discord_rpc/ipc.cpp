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
// Discord's SET_ACTIVITY rate limit: 5 per 20 s (sliding window). A change goes out at once while a
// slot is free, else when the oldest of the last 5 sends is 20 s old; the newest presence wins.
constexpr int kSendBurst = 5;
constexpr unsigned long long kSendWindowMs = 20000;
constexpr unsigned long long kSendMarginMs = 250;       // slack on top of the window (clock/queue skew)
constexpr unsigned long long kSettleMs = 300;           // a burst of changes (Newbucks while selling) is one send...
constexpr unsigned long long kSettleMaxMs = 1000;       //   ... but a steady stream still goes out every second
constexpr unsigned long long kRetryMinMs = 5000;        // Discord not running: first retry
constexpr unsigned long long kRetryMaxMs = 30000;       //   ... backing off to this
constexpr unsigned long long kRejectedRetryMs = 120000; // application id refused (unless it changes)
constexpr DWORD kHandshakeMs = 3000;
constexpr uint32_t kMaxFrame = 64 * 1024;
constexpr size_t kMaxField = 128;                       // Discord's limit for details / state
// The large image: Slime Rancher 2's square cover art from its official Discord application
// (233 x 233; the app icon is only 32 x 32). Discord accepts https URLs as activity assets.
constexpr const char* kLargeImage =
    "https://cdn.discordapp.com/app-icons/1124351953024077845/3bcb29b4113fd3799b70218aafe5385d.png?size=512";
constexpr const char* kLargeText = "Slime Rancher 2";

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
int g_pipeIndex = -1;         // N of \\.\pipe\discord-ipc-N while connected
std::string g_lastStatus;     // what was last logged / shown
std::string g_lastError;      // last ERROR reply logged (one line per distinct message)
unsigned long long g_sendTimes[kSendBurst] = {};  // ring of the last SET_ACTIVITY ticks (rate limit)
int g_sendNext = 0;                               // oldest entry = the next one overwritten
bool g_noDisplayType = false; // Discord refused status_display_type once: leave it out from then on
bool g_noAssets = false;      // Discord refused the image URL once: leave the assets out from then on
bool g_resend = false;        // send the current presence again (after dropping a refused field)

// The card's Status line: a short text, the technical detail for its tooltip and when the next
// connection attempt is (the card counts it down). Logged (text and detail) when the text changes.
void Status(const std::string& text, bridge::Level level, const std::string& detail = {},
            unsigned long long retryAt = 0, bool log = true) {
    bridge::SetStatus({text, level, detail, retryAt});
    if (text == g_lastStatus) return;
    g_lastStatus = text;
    if (!log) return;
    const std::string line = detail.empty() ? text : std::format("{} ({})", text, detail);
    if (level == bridge::Level::Error || level == bridge::Level::Warn) SR2_WARN("[{}] {}", kModule, line);
    else SR2_INFO("[{}] {}", kModule, line);
}

// Rate limit (kSendBurst per kSendWindowMs): the tick from which the next SET_ACTIVITY may go out.
unsigned long long NextSlot() {
    const unsigned long long oldest = g_sendTimes[g_sendNext];
    return oldest == 0 ? 0 : oldest + kSendWindowMs + kSendMarginMs;
}

void NoteSend(unsigned long long now) {
    g_sendTimes[g_sendNext] = now;
    g_sendNext = (g_sendNext + 1) % kSendBurst;
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
    g_pipeIndex = -1;
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

// detail: Ok = the user's name; Rejected = Discord's short reason. tech: the technical detail (tooltip).
ConnectResult Connect(const std::string& id, std::string& detail, std::string& tech) {
    for (int i = 0; i < 10 && !Connected(); ++i) {
        const std::wstring name = std::format(L"\\\\.\\pipe\\discord-ipc-{}", i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) g_pipe = h, g_pipeIndex = i;
    }
    if (!Connected()) {
        tech = "No Discord IPC pipe open (discord-ipc-0 to -9): the Discord desktop app is not running.";
        return ConnectResult::NoDiscord;
    }
    const std::string pipe = std::format("discord-ipc-{}", g_pipeIndex);
    json::Value hs = json::Value::Object();
    hs.set("v", json::Value(1.0));
    hs.set("client_id", json::Value(id));
    uint32_t op = 0;
    std::string reply;
    if (!WriteFrame(kHandshake, Compact(hs)) || ReadFrame(op, reply, kHandshakeMs) != 1) {
        Disconnect();
        tech = std::format("Handshake on {} failed: no reply within {} s.", pipe, kHandshakeMs / 1000);
        return ConnectResult::Failed;
    }
    json::Value doc;
    std::string perr;
    json::Parse(reply, doc, perr);
    if (op == kClose) {
        Disconnect();
        detail = doc["message"].asString("closed");
        tech = std::format("Discord closed the handshake on {} with code {}: {}", pipe, doc["code"].asInt(), detail);
        return ConnectResult::Rejected;
    }
    if (op != kFrame || doc["evt"].asString() != "READY") {
        Disconnect();
        tech = std::format("Handshake on {} failed: unexpected reply (op {}, {}).", pipe, op,
                           doc["evt"].asString("no event"));
        return ConnectResult::Failed;
    }
    const json::Value& user = doc["data"]["user"];
    detail = user["global_name"].asString();
    if (detail.empty()) detail = user["username"].asString();
    tech = std::format("Handshake OK on {} (application {}).", pipe, id);
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
        if (!g_noAssets) {
            json::Value assets = json::Value::Object();
            assets.set("large_image", json::Value(std::string(kLargeImage)));
            assets.set("large_text", json::Value(std::string(kLargeText)));
            act.set("assets", std::move(assets));
        }
        // What the member list shows after "Playing" (newer Discord clients; older ones ignore or refuse it).
        if (!g_noDisplayType && p->displayType > 0)
            act.set("status_display_type", json::Value(static_cast<double>(p->displayType)));
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
                if (!g_noDisplayType && msg.find("status_display_type") != std::string::npos) {
                    g_noDisplayType = true;  // this Discord does not know the field: send without it
                    g_resend = true;
                    SR2_INFO("[{}] Discord does not accept status_display_type; the member list shows the app name",
                             kModule);
                } else if (!g_noAssets && (msg.find("large_image") != std::string::npos ||
                                           msg.find("assets") != std::string::npos)) {
                    g_noAssets = true;  // the image URL was refused: send without the image
                    g_resend = true;
                    SR2_INFO("[{}] Discord refused the image; the presence is sent without it", kModule);
                }
                if (msg != g_lastError) {
                    g_lastError = msg;
                    SR2_WARN("[{}] Discord refused the presence: {}", kModule, msg);
                }
            }
        }
    }
    return true;
}

// "Refused: <reason>" stays one short line; the full message is in the tooltip.
std::string ShortReason(std::string s) {
    constexpr size_t kMax = 40;
    if (s.size() <= kMax) return s;
    size_t n = kMax;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s + "...";
}

DWORD WINAPI Worker(LPVOID) {
    unsigned long long nextTry = 0, backoff = kRetryMinMs, sentVersion = 0, sentCount = 0;
    unsigned long long seenVersion = 0, changedAt = 0, pendingSince = 0;  // settling a burst of changes
    DWORD waitMs = kTickMs;
    std::string rejectedId, user, connTech;
    for (;;) {
        WaitForSingleObject(g_wake, waitMs);
        waitMs = kTickMs;
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
        // Slime Rancher 2's own application unless turned off, else the user's id from the card.
        const bool official = settings::GetBool(bridge::kModule, bridge::kKeyOfficial);
        const std::string id =
            official ? std::string(bridge::kOfficialAppId) : Trim(settings::GetString(bridge::kModule, bridge::kKeyAppId));
        const unsigned long long now = GetTickCount64();
        const bool reconnect = bridge::TakeReconnect();
        auto drop = [&] {
            if (Connected()) {
                if (SendActivity(nullptr)) NoteSend(GetTickCount64());
                Disconnect();
                SR2_INFO("[{}] disconnected from Discord", kModule);
            }
        };
        auto lost = [&] {  // the pipe broke (Discord quit or restarted)
            Disconnect();
            nextTry = now + kRetryMinMs;
            Status("Reconnecting...", bridge::Level::Dim,
                   "Discord closed the connection (the pipe broke or it sent CLOSE); connecting again.", nextTry);
        };
        if (!active) {
            drop();
            Status("Off", bridge::Level::Dim, {}, 0, false);
            nextTry = 0, backoff = kRetryMinMs;
            continue;
        }
        if (id.empty() || !ValidId(id)) {
            drop();
            if (id.empty())
                Status("No application id", bridge::Level::Warn,
                       "Paste your Discord application id below, or turn on \"Show as Slime Rancher 2\".");
            else
                Status("Invalid application id", bridge::Level::Warn,
                       "The application id is the number (about 18-20 digits) from the Discord Developer Portal.");
            continue;
        }
        if (Connected() && id != g_connectedId) {
            drop();
            nextTry = 0, backoff = kRetryMinMs;
        }
        if (reconnect) {  // the card's Reconnect button: drop and try again now, whatever the wait
            SR2_INFO("[{}] reconnect requested", kModule);
            drop();
            rejectedId.clear();
            nextTry = 0, backoff = kRetryMinMs;
            g_noDisplayType = g_noAssets = false;
        }
        if (!Connected()) {
            if (!rejectedId.empty() && id != rejectedId) {  // a new id after a refusal: try it at once
                rejectedId.clear();
                nextTry = 0;
            }
            if (now < nextTry) continue;
            std::string detail, tech;
            switch (Connect(id, detail, tech)) {
                case ConnectResult::Ok:
                    rejectedId.clear();
                    backoff = kRetryMinMs;
                    sentVersion = 0;  // send the current presence again
                    pendingSince = 0;
                    user = detail;
                    connTech = tech + (official ? " Slime Rancher 2's own application." : " Your own application.");
                    break;
                case ConnectResult::NoDiscord:
                    nextTry = now + backoff;
                    backoff = std::min(backoff * 2, kRetryMaxMs);
                    Status("Discord not running", bridge::Level::Dim, tech, nextTry);
                    continue;
                case ConnectResult::Rejected:
                    rejectedId = id;
                    nextTry = now + kRejectedRetryMs;
                    Status("Refused: " + ShortReason(detail), bridge::Level::Error,
                           tech + "\nTried again in 2 minutes, at once when the application id changes, or with Reconnect.",
                           nextTry);
                    continue;
                case ConnectResult::Failed:
                    nextTry = now + backoff;
                    backoff = std::min(backoff * 2, kRetryMaxMs);
                    Status("Reconnecting...", bridge::Level::Warn, tech, nextTry);
                    continue;
            }
        }
        if (!Poll()) {
            lost();
            continue;
        }
        if (g_resend) {  // a field was refused: the next send goes out without it
            g_resend = false;
            sentVersion = 0;
        }
        // Send as soon as a change has settled and Discord's rate limit has a free slot; the newest wins.
        if (have && version != seenVersion) {
            seenVersion = version;
            changedAt = now;
            if (!pendingSince) pendingSince = now;
        }
        unsigned long long slotWait = 0;
        if (have && version != sentVersion) {
            if (!pendingSince) pendingSince = changedAt = now;  // a resend (reconnect, refused field)
            const unsigned long long settleAt = std::min(changedAt + kSettleMs, pendingSince + kSettleMaxMs);
            const unsigned long long sendAt = std::max(settleAt, NextSlot());
            if (now >= sendAt) {
                if (!SendActivity(&want)) {
                    lost();
                    continue;
                }
                NoteSend(now);
                sentVersion = version;
                pendingSince = 0;
                ++sentCount;
                SR2_DEBUG("[{}] presence: {} | {}", kModule, want.details, want.state);
            } else {
                waitMs = static_cast<DWORD>(std::min<unsigned long long>(kTickMs, sendAt - now));
                if (NextSlot() > now) slotWait = NextSlot() - now;
            }
        }
        std::string tech = std::format("{}\nPresence updates sent: {} (Discord allows {} per {} s).", connTech, sentCount,
                                       kSendBurst, kSendWindowMs / 1000);
        if (slotWait) tech += std::format("\nThe latest change goes out in {} s (rate limit).", (slotWait + 999) / 1000);
        if (!g_lastError.empty()) tech += "\nLast error from Discord: " + g_lastError;
        Status(user.empty() ? std::string("Connected") : "Connected as " + user, bridge::Level::Ok, tech);
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
