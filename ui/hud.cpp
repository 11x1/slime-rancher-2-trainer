#include "hud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#if defined(SR2_HAVE_IMGUI)
#include "imgui.h"
#endif

namespace sr2::ui::hud {
namespace {

struct Entry {
    Frame frame;
    unsigned long long tick = 0;
};

std::mutex g_mutex;
std::map<std::string, Entry, std::less<>> g_frames;

bool Fresh(const Entry& e, unsigned long long now) { return now - e.tick <= kHudStaleMs; }

}  // namespace

void Publish(std::string_view owner, Frame frame) {
    unsigned long long now = GetTickCount64();
    std::lock_guard lk(g_mutex);
    auto it = g_frames.find(owner);
    if (it == g_frames.end()) it = g_frames.emplace(std::string(owner), Entry{}).first;
    it->second.frame = std::move(frame);
    it->second.tick = now;
}

void Clear(std::string_view owner) {
    std::lock_guard lk(g_mutex);
    auto it = g_frames.find(owner);
    if (it != g_frames.end()) g_frames.erase(it);
}

bool HasContent() {
    unsigned long long now = GetTickCount64();
    std::lock_guard lk(g_mutex);
    for (const auto& [k, e] : g_frames)
        if (!e.frame.markers.empty() && Fresh(e, now)) return true;
    return false;
}

#if defined(SR2_HAVE_IMGUI)

namespace {

// clip = vp * (x, y, z, 1); vp is column-major: element (row r, col c) = vp[c * 4 + r]
bool Project(const float* vp, float x, float y, float z, float w, float h, ImVec2& out) {
    float cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    float cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    float cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (!(cw > 0.05f) || !std::isfinite(cx) || !std::isfinite(cy)) return false;  // behind the camera
    float nx = cx / cw, ny = cy / cw;
    if (std::fabs(nx) > 1.05f || std::fabs(ny) > 1.05f) return false;  // off screen
    out = ImVec2((nx * 0.5f + 0.5f) * w, (1.0f - (ny * 0.5f + 0.5f)) * h);
    return true;
}

ImU32 WithAlpha(ImU32 c, float a) {
    unsigned alpha = static_cast<unsigned>(std::clamp(a, 0.0f, 1.0f) * ((c >> IM_COL32_A_SHIFT) & 0xFF) + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

void DrawMarker(ImDrawList* dl, ImVec2 p, float r, ImU32 fill, float alpha, const Frame& f, const Marker& m) {
    const ImU32 ink = IM_COL32(15, 10, 30, static_cast<int>(220 * alpha));
    const ImU32 col = WithAlpha(fill, alpha);
    const ImU32 hi = IM_COL32(255, 255, 255, static_cast<int>(200 * alpha));
    // diamond with a dark outline, a light inner diamond and a "crack" through it
    ImVec2 o[4] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}};
    float ro = r + std::max(1.5f, r * 0.22f);
    ImVec2 oo[4] = {{p.x, p.y - ro}, {p.x + ro, p.y}, {p.x, p.y + ro}, {p.x - ro, p.y}};
    dl->AddQuadFilled(oo[0], oo[1], oo[2], oo[3], ink);
    dl->AddQuadFilled(o[0], o[1], o[2], o[3], col);
    float ri = r * 0.45f;
    dl->AddQuad({p.x, p.y - ri}, {p.x + ri, p.y}, {p.x, p.y + ri}, {p.x - ri, p.y}, hi, std::max(1.0f, r * 0.12f));
    float t = std::max(1.0f, r * 0.14f);
    dl->AddLine({p.x - r * 0.15f, p.y - r * 0.85f}, {p.x + r * 0.12f, p.y - r * 0.2f}, ink, t);
    dl->AddLine({p.x + r * 0.12f, p.y - r * 0.2f}, {p.x - r * 0.1f, p.y + r * 0.25f}, ink, t);
    dl->AddLine({p.x - r * 0.1f, p.y + r * 0.25f}, {p.x + r * 0.15f, p.y + r * 0.85f}, ink, t);

    ImFont* font = ImGui::GetFont();
    float fs = std::max(11.0f, r * 1.25f);
    auto label = [&](const char* text, float y) {
        ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, text);
        ImVec2 at(p.x - ts.x * 0.5f, y);
        dl->AddText(font, fs, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), text);
        dl->AddText(font, fs, at, IM_COL32(255, 255, 255, static_cast<int>(235 * alpha)), text);
    };
    if (!f.tag.empty()) label(f.tag.c_str(), p.y - ro - fs - 2.0f);
    if (f.showDistance) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f m", m.distance);
        label(buf, p.y + ro + 2.0f);
    }
}

}  // namespace

void Draw() {
    // copy out under the lock, draw without it
    std::vector<Frame> frames;
    {
        unsigned long long now = GetTickCount64();
        std::lock_guard lk(g_mutex);
        for (const auto& [k, e] : g_frames)
            if (!e.frame.markers.empty() && Fresh(e, now)) frames.push_back(e.frame);
    }
    if (frames.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w <= 0 || h <= 0) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const Frame& f : frames) {
        float r = std::clamp(h * 0.0105f * f.size, 4.0f, 64.0f);
        for (const Marker& m : f.markers) {
            ImVec2 p;
            if (!Project(f.vp, m.x, m.y, m.z, w, h, p)) continue;
            float k = f.range > 1 ? std::clamp(m.distance / f.range, 0.0f, 1.0f) : 0.0f;
            float alpha = 1.0f - 0.5f * k;          // fades to half towards the range limit
            float rr = r * (1.15f - 0.35f * k);      // and shrinks a little
            DrawMarker(dl, p, rr, f.color, alpha, f, m);
        }
    }
}

#else

void Draw() {}

#endif

}  // namespace sr2::ui::hud
