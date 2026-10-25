#include "autotest_view.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#if defined(SR2_HAVE_IMGUI)
#include "imgui.h"
#include "imgui_internal.h"
#include "theme.h"
#endif

namespace sr2::ui::autotest {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;            // answers and screenshots
std::atomic<bool> g_armed{false};
std::optional<Question> g_question;      // guarded by g_mutex
std::map<uint64_t, Answer> g_answers;    // finished, not yet taken (guarded by g_mutex)
std::atomic<bool> g_hasQuestion{false};
std::atomic<bool> g_pointer{false}, g_typing{false};

// Screenshots: one caller at a time (g_shotTurn); the render thread fills g_shot.
std::mutex g_shotTurn;
int g_shotPhase = 0;                     // 0 none, 1 before the overlay, 2 after (guarded by g_mutex)
std::atomic<int> g_shotWanted{0};        // copy of g_shotPhase for the render thread's cheap check
std::atomic<bool> g_shotHide{false};     // the pending shot wants the frame without the tag / panel / edges
std::atomic<bool> g_frameHidden{false};  // render thread: this frame's Draw left them out
std::optional<Shot> g_shot;              // guarded by g_mutex

// Mod menu page requests (RequestPage); guarded by g_mutex.
std::optional<PageRequest> g_pageReq;
uint64_t g_pageNext = 0;
std::map<uint64_t, std::pair<int, std::string>> g_pageDone;  // id -> (1 / -1, detail)

void Finish(uint64_t id, Answer a) {
    a.id = id;
    g_answers[id] = std::move(a);
    if (g_answers.size() > 32) g_answers.erase(g_answers.begin());  // never taken (waiter gone)
    g_question.reset();
    g_hasQuestion = false;
    g_pointer = false;
    g_typing = false;
}

}  // namespace

void SetArmed(bool armed) { g_armed.store(armed); }
bool Armed() { return g_armed.load(); }

void Ask(const Question& q) {
    {
        std::lock_guard lk(g_mutex);
        if (g_question) {
            Answer a;
            a.cancelled = true;
            Finish(g_question->id, std::move(a));
        }
        g_question = q;
        g_hasQuestion = true;
    }
    g_cv.notify_all();
}

void CancelAsk(uint64_t id) {
    {
        std::lock_guard lk(g_mutex);
        if (!g_question || (id != 0 && g_question->id != id)) return;
        Answer a;
        a.cancelled = true;
        Finish(g_question->id, std::move(a));
    }
    g_cv.notify_all();
}

bool WaitAnswer(uint64_t id, long long timeoutMs, Answer& out, const std::atomic<bool>* stop) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    std::unique_lock lk(g_mutex);
    for (;;) {
        if (auto it = g_answers.find(id); it != g_answers.end()) {
            out = std::move(it->second);
            g_answers.erase(it);
            return true;
        }
        if (stop && stop->load()) return false;
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= end) return false;
        g_cv.wait_for(lk, std::chrono::milliseconds(50));
    }
}

bool QuestionOpen() { return g_hasQuestion.load(); }

Shot TakeShot(bool withOverlay, long long timeoutMs, bool hideHarness) {
    std::lock_guard turn(g_shotTurn);
    std::unique_lock lk(g_mutex);
    g_shot.reset();
    g_shotPhase = withOverlay ? 2 : 1;
    g_shotHide = withOverlay && hideHarness;
    g_shotWanted = g_shotPhase;
    const bool got = g_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return g_shot.has_value(); });
    g_shotPhase = 0;
    g_shotWanted = 0;
    g_shotHide = false;
    if (!got) {
        Shot s;
        s.error = "no frame was presented in time (minimised, or the overlay is unavailable)";
        return s;
    }
    Shot s = std::move(*g_shot);
    g_shot.reset();
    return s;
}

bool HasContent() { return g_armed.load(std::memory_order_relaxed) || g_hasQuestion.load(std::memory_order_relaxed); }
bool WantsInput() { return g_hasQuestion.load(std::memory_order_relaxed); }
bool PointerOnPanel() { return g_pointer.load(std::memory_order_relaxed); }
bool Typing() { return g_typing.load(std::memory_order_relaxed); }
bool WantsGameInputLock() { return g_hasQuestion.load() && (g_pointer.load() || g_typing.load()); }

bool ShotWanted(bool afterOverlay) {
    const int w = g_shotWanted.load(std::memory_order_relaxed);
    if (w != (afterOverlay ? 2 : 1)) return false;
    // Without the harness bits: only a frame whose Draw left them out (or that had none to draw). A shot
    // asked for between Draw and this check waits for the next frame.
    return !afterOverlay || !g_shotHide.load() || g_frameHidden.load() || !HasContent();
}

uint64_t RequestPage(const std::string& cat, const std::string& tab) {
    std::lock_guard lk(g_mutex);
    PageRequest r;
    r.id = ++g_pageNext;
    r.cat = cat;
    r.tab = tab;
    g_pageReq = r;  // replaces an older one not applied yet
    return r.id;
}

int PageResult(uint64_t id, std::string& detail) {
    std::lock_guard lk(g_mutex);
    auto it = g_pageDone.find(id);
    if (it == g_pageDone.end()) return 0;
    detail = it->second.second;
    const int r = it->second.first;
    g_pageDone.erase(it);
    return r;
}

bool TakePageRequest(PageRequest& out) {
    std::lock_guard lk(g_mutex);
    if (!g_pageReq) return false;
    out = std::move(*g_pageReq);
    g_pageReq.reset();
    return true;
}

void PageDone(uint64_t id, bool ok, const std::string& detail) {
    std::lock_guard lk(g_mutex);
    g_pageDone[id] = {ok ? 1 : -1, detail};
    if (g_pageDone.size() > 16) g_pageDone.erase(g_pageDone.begin());  // never asked for (caller gone)
}

void DeliverShot(Shot shot) {
    {
        std::lock_guard lk(g_mutex);
        if (g_shotPhase == 0) return;  // the caller gave up
        g_shot = std::move(shot);
        g_shotWanted = 0;
    }
    g_cv.notify_all();
}

#if defined(SR2_HAVE_IMGUI)

namespace {

// Render thread only.
uint64_t g_drawnId = 0;   // the question the note buffer belongs to
char g_note[256] = {};
bool g_keyDown[4] = {};   // keys 1-4 (top row or numpad) held last frame

ImU32 WithAlpha(ImU32 c, float a) {
    const unsigned alpha = static_cast<unsigned>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha > 255 ? 255 : alpha) << IM_COL32_A_SHIFT);
}

void Choose(uint64_t id, const std::string& label) {
    {
        std::lock_guard lk(g_mutex);
        if (!g_question || g_question->id != id) return;
        Answer a;
        a.choice = label;
        a.note = g_note;
        Finish(id, std::move(a));
    }
    g_cv.notify_all();
}

void DrawTag(float s) {
    const theme::Palette& P = theme::Colors();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x - 16 * s, disp.y - 16 * s), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.pill);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 6 * s));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##sr2mods_autotest_tag", nullptr, f)) {
        const float r = 4 * s;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, P.accent);
        ImGui::Dummy(ImVec2(2 * r + 6 * s, h));
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, P.soft);
        ImGui::TextUnformatted("Test harness connected");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// Pulsing border on top of everything (foreground draw list), ~1 Hz.
void DrawEdges(float s) {
    const theme::Palette& P = theme::Colors();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const double t = static_cast<double>(GetTickCount64() % 100000) / 1000.0;
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(t * 2.0 * 3.14159265358979));
    const ImU32 c = WithAlpha(P.accent, 0.25f + 0.75f * pulse);
    const float w = 12 * s;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(disp.x, w), c);
    dl->AddRectFilled(ImVec2(0, disp.y - w), ImVec2(disp.x, disp.y), c);
    dl->AddRectFilled(ImVec2(0, w), ImVec2(w, disp.y - w), c);
    dl->AddRectFilled(ImVec2(disp.x - w, w), ImVec2(disp.x, disp.y - w), c);
}

void DrawQuestion(const Question& q, float s, bool foreground, bool overlayTyping) {
    const theme::Palette& P = theme::Colors();
    if (q.id != g_drawnId) {
        g_drawnId = q.id;
        g_note[0] = 0;
        for (bool& k : g_keyDown) k = true;  // a key held when the question opened is not an answer
    }
    // Keys 1-4 (top row or numpad), on the press, not while a text field has the keyboard.
    const bool typing = g_typing.load();
    for (int i = 0; i < 4; ++i) {
        const bool down = foreground && ((GetAsyncKeyState('1' + i) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD1 + i) & 0x8000));
        if (down && !g_keyDown[i] && !typing && !overlayTyping && i < static_cast<int>(q.options.size())) {
            g_keyDown[i] = down;
            Choose(q.id, q.options[static_cast<size_t>(i)]);
            return;
        }
        g_keyDown[i] = down;
    }

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, 28 * s), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360 * s, 0), ImVec2(640 * s, disp.y * 0.6f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.win);
    ImGui::PushStyleColor(ImGuiCol_Border, P.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 12 * s));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    bool pointer = false, noteActive = false;
    if (ImGui::Begin("##sr2mods_autotest_question", nullptr, f)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());  // above the settings window
        ImGui::PushFont(theme::Display());
        ImGui::PushStyleColor(ImGuiCol_Text, P.accent);
        ImGui::TextUnformatted("Test question");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600 * s);
        ImGui::TextUnformatted(q.text.c_str());
        if (!q.caseId.empty() || q.step > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::SetWindowFontScale(0.85f);
            if (!q.caseId.empty() && q.step > 0) ImGui::Text("Case %s, step %d", q.caseId.c_str(), q.step);
            else if (!q.caseId.empty()) ImGui::Text("Case %s", q.caseId.c_str());
            else ImGui::Text("Step %d", q.step);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 4 * s));
        for (size_t i = 0; i < q.options.size(); ++i) {
            if (i) ImGui::SameLine(0, 8 * s);
            const std::string label = std::to_string(i + 1) + "  " + q.options[i] + "##opt" + std::to_string(i);
            if (ImGui::Button(label.c_str())) {
                Choose(q.id, q.options[i]);
                // Close the window cleanly this frame; the question is gone from the next one.
            }
        }
        ImGui::Dummy(ImVec2(0, 2 * s));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##note", "Optional note, sent with the answer", g_note, sizeof(g_note));
        noteActive = ImGui::IsItemActive();
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextUnformatted("Keys 1-4 answer. In game the mouse needs a free cursor: open the mod menu or a game menu.");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        pointer = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    g_pointer = pointer;
    g_typing = noteActive;
}

}  // namespace

void Draw(float scale, bool foreground, bool overlayTyping) {
    const float s = scale > 0 ? scale : 1.0f;
    // A screenshot with the overlay but without the harness bits is pending: leave them out this frame.
    const bool hide = g_shotWanted.load() == 2 && g_shotHide.load();
    g_frameHidden = hide;
    if (hide) return;
    std::optional<Question> q;
    {
        std::lock_guard lk(g_mutex);
        q = g_question;
    }
    if (q) {
        DrawEdges(s);
        DrawQuestion(*q, s, foreground, overlayTyping);
    } else {
        g_pointer = false;
        g_typing = false;
    }
    if (g_armed.load()) DrawTag(s);
}

#else  // !SR2_HAVE_IMGUI

void Draw(float, bool, bool) {}

#endif

}  // namespace sr2::ui::autotest
