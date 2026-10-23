#include "momentum_hud.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "core/settings.h"
#include "hud_edit.h"
#include "overlay.h"

#if defined(SR2_HAVE_IMGUI)
#include "imgui.h"
#endif

namespace sr2::ui::momentum_hud {
namespace {

constexpr float kStillSpeed = 10.0f;  // u/s: below this counts as standing still (fade)
constexpr float kFadeDelay = 0.6f;    // s still before the fade starts
constexpr float kFadeTime = 0.35f;    // s for a full fade in / out

std::atomic<unsigned long long> g_stamp{0};  // GetTickCount64 of the last publish, 0 = cleared
std::atomic<float> g_speed{0}, g_trend{0}, g_pre{0}, g_preDelta{0}, g_sync{-1};
std::atomic<float> g_x{50}, g_y{57.1f}, g_size{1.6f}, g_barFull{640};
std::atomic<bool> g_details{false}, g_bar{false}, g_fade{true};

// Render thread only.
float g_alpha = 0;       // current opacity
float g_stillTime = 0;   // seconds below kStillSpeed
float g_tint = 0;        // smoothed -1 (losing) .. +1 (gaining)

// hl2_movement's settings for the edit-mode preview (the module does not
// publish while the overlay has the input).
constexpr const char* kOwner = "hl2_movement";
constexpr const char* kKeyBar = "momentum_hud_bar";
constexpr const char* kKeyDetails = "momentum_hud_details";
constexpr float kSampleSpeed = 320;  // shown in the edit mode when there is no live speed

bool Fresh() {
    const unsigned long long s = g_stamp.load(std::memory_order_acquire);
    return s != 0 && GetTickCount64() - s <= kStaleMs;
}

float TargetAlpha() {
    if (!g_fade.load(std::memory_order_relaxed)) return 1.0f;
    return g_stillTime >= kFadeDelay ? 0.0f : 1.0f;
}

}  // namespace

void Publish(const State& s) {
    constexpr auto r = std::memory_order_relaxed;
    g_speed.store(s.speed, r);
    g_trend.store(s.trend, r);
    g_pre.store(s.preSpeed, r);
    g_preDelta.store(s.preDelta, r);
    g_sync.store(s.sync, r);
    g_details.store(s.details, r);
    g_bar.store(s.bar, r);
    g_fade.store(s.fade, r);
    g_x.store(s.x, r);
    g_y.store(s.y, r);
    g_size.store(s.size, r);
    g_barFull.store(s.barFull, r);
    g_stamp.store(std::max<unsigned long long>(1, GetTickCount64()), std::memory_order_release);
}

void Clear() { g_stamp.store(0, std::memory_order_release); }

void EditBox(float sizePct, float screenH, float& w, float& h) {
    namespace settings = ::sr2::core::settings;
    const float fs = screenH * sizePct / 100.0f;
    w = fs * kBoxW;
    h = fs * BoxHeightUnits(settings::GetBool(kOwner, kKeyBar), settings::GetBool(kOwner, kKeyDetails));
}

bool HasContent() {
    if (hud_edit::Editing(hud_edit::Element::SpeedPanel)) return true;  // drawn while being placed
    if (!Fresh() || overlay::IsOpen()) {
        g_alpha = 0;  // re-appears with a short fade-in
        return false;
    }
    // Fully faded out and still standing still: nothing to draw. Moving again
    // makes the target 1, Draw() then runs and fades back in.
    if (g_alpha <= 0.001f && g_fade.load(std::memory_order_relaxed) &&
        g_speed.load(std::memory_order_relaxed) < kStillSpeed)
        return false;
    return true;
}

#if defined(SR2_HAVE_IMGUI)

namespace {

ImU32 Col(float r, float g, float b, float a) {
    return IM_COL32(static_cast<int>(r * 255.0f + 0.5f), static_cast<int>(g * 255.0f + 0.5f),
                    static_cast<int>(b * 255.0f + 0.5f), static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// Neutral -> subtle green (t > 0) or red (t < 0).
void TintRgb(float t, float& r, float& g, float& b) {
    const float n = 0.93f;
    t = std::clamp(t, -1.0f, 1.0f);
    if (t >= 0) {
        r = n + (0.55f - n) * t;
        g = n + (1.00f - n) * t;
        b = n + (0.62f - n) * t;
    } else {
        t = -t;
        r = n + (1.00f - n) * t;
        g = n + (0.52f - n) * t;
        b = n + (0.50f - n) * t;
    }
}

// Maps a speed change to -1..1 with a dead zone (steady = neutral).
float TrendToTint(float delta, float speed) {
    const float dead = std::max(2.0f, speed * 0.01f);
    const float a = std::fabs(delta);
    if (a <= dead) return 0.0f;
    const float k = std::clamp((a - dead) / std::max(20.0f, speed * 0.08f), 0.0f, 1.0f);
    return delta > 0 ? k : -k;
}

void ShadowText(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, float alpha, const char* text) {
    const float o = std::max(1.0f, std::round(size / 14.0f));
    dl->AddText(font, size, ImVec2(p.x + o, p.y + o), Col(0, 0, 0, 0.55f * alpha), text);
    dl->AddText(font, size, p, col, text);
}

}  // namespace

void Draw() {
    constexpr auto r = std::memory_order_relaxed;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w <= 0 || h <= 0) return;
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    const bool fresh = Fresh();
    const bool editing = hud_edit::Editing(hud_edit::Element::SpeedPanel);
    if (!fresh && !editing) return;
    const float speed = fresh ? g_speed.load(r) : kSampleSpeed;
    bool bar = g_bar.load(r), details = g_details.load(r);
    if (editing) {
        namespace settings = ::sr2::core::settings;
        bar = settings::GetBool(kOwner, kKeyBar);
        details = settings::GetBool(kOwner, kKeyDetails);
    }
    hud_edit::Placement place{g_x.load(r), g_y.load(r), g_size.load(r)};
    hud_edit::Preview(hud_edit::Element::SpeedPanel, place);  // while editing, and until a commit has landed

    if (editing) {
        g_alpha = 1.0f;
        g_stillTime = 0;
    } else {
        g_stillTime = speed < kStillSpeed ? g_stillTime + dt : 0.0f;
        const float target = TargetAlpha();
        const float step = dt / kFadeTime;
        g_alpha = g_alpha < target ? std::min(target, g_alpha + step) : std::max(target, g_alpha - step);
        if (g_alpha <= 0.001f) return;
    }

    // Colour follows the speed trend, smoothed (~0.12 s) so it never flickers.
    const float want = fresh ? TrendToTint(g_trend.load(r), speed) : 0.0f;
    g_tint += (want - g_tint) * std::min(1.0f, dt / 0.12f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = std::max(4.0f, h * std::clamp(place.size, 0.2f, 20.0f) / 100.0f);
    const float boxW = size * kBoxW, boxH = size * BoxHeightUnits(bar, details);
    const float cx = hud_edit::ToPx(place.x, w, boxW) + boxW * 0.5f;
    float y = hud_edit::ToPx(place.y, h, boxH);

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(speed)));
    float cr, cg, cb;
    TintRgb(g_tint, cr, cg, cb);
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
    // Each text is centred on its own line height (in font sizes), so the box
    // does not depend on the font's metrics.
    ShadowText(dl, font, size, ImVec2(std::round(cx - ts.x * 0.5f), std::round(y + (size * kLine - ts.y) * 0.5f)),
               Col(cr, cg, cb, 0.9f * g_alpha), g_alpha, buf);
    y += size * kLine;

    if (bar) {
        const float bw = std::round(size * kBoxW), bh = std::max(1.0f, std::round(size / 12.0f));
        const float frac = std::clamp(speed / std::max(1.0f, g_barFull.load(r)), 0.0f, 1.0f);
        const float x0 = std::round(cx - bw * 0.5f), y0 = std::round(y + (size * kBarH - bh) * 0.5f);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), Col(0, 0, 0, 0.25f * g_alpha));
        if (frac > 0)
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw * frac, y0 + bh), Col(cr, cg, cb, 0.7f * g_alpha));
        y += size * kBarH;
    }

    if (details) {
        const float pre = fresh ? g_pre.load(r) : kSampleSpeed, sync = fresh ? g_sync.load(r) : 80.0f;
        const int ip = static_cast<int>(std::lround(pre)), is = static_cast<int>(std::lround(sync));
        char line[64];
        line[0] = 0;
        if (pre > 0 && sync >= 0) std::snprintf(line, sizeof(line), "%d   %d%%", ip, is);
        else if (pre > 0) std::snprintf(line, sizeof(line), "%d", ip);
        else if (sync >= 0) std::snprintf(line, sizeof(line), "%d%%", is);
        if (line[0]) {
            const float s2 = size * 0.72f;
            float pr, pg, pb;
            TintRgb(fresh ? TrendToTint(g_preDelta.load(r), pre) * 0.8f : 0.0f, pr, pg, pb);
            const ImVec2 ls = font->CalcTextSizeA(s2, FLT_MAX, 0.0f, line);
            ShadowText(dl, font, s2,
                       ImVec2(std::round(cx - ls.x * 0.5f), std::round(y + (size * kDetailsH - ls.y) * 0.5f)),
                       Col(pr, pg, pb, 0.7f * g_alpha), g_alpha, line);
        }
    }
}

#else

void Draw() {}

#endif

}  // namespace sr2::ui::momentum_hud
