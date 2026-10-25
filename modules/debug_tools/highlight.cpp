// debug_tools: debug highlighter (see highlight.h).

#include "modules/debug_tools/highlight.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "modules/common/debug_marks.h"
#include "modules/debug_tools/hitboxes.h"
#include "ui/hud.h"
#include "ui/overlay.h"

namespace sr2::modules::debug_tools::highlight {
namespace {

namespace dm = sr2::modules::common::debug_marks;
namespace hud = sr2::ui::hud;

constexpr const char* kLinesOwner = "debug_tools.highlight";
constexpr float kMaxBoxDistance = 250.0f;   // boxes farther than this from the camera are not drawn
constexpr float kMaxLabelDistance = 80.0f;  // labels only this close (dozens of ice crystals otherwise)
constexpr size_t kMaxBoxes = 600;           // per frame, nearest categories first in registry order
constexpr int kRingSegments = 48;
constexpr unsigned long long kLogEveryMs = 10000;

struct State {
    bool on = false;
    std::set<std::string> owners;  // label frames published (hud owners)
    std::string logged;
    unsigned long long lastLog = 0;
};
State g;

void ClearAll() {
    hud::Clear(kLinesOwner);
    for (const std::string& o : g.owners) hud::Clear(o);
    g.owners.clear();
}

void AddLine(std::vector<hud::Line>& out, float ax, float ay, float az, float bx, float by, float bz, uint32_t color) {
    out.push_back({ax, ay, az, bx, by, bz, color});
}

void AddBox(std::vector<hud::Line>& out, const dm::Box& b, uint32_t color) {
    float p[8][3];
    for (int i = 0; i < 8; ++i) {
        p[i][0] = b.x + ((i & 1) ? b.hx : -b.hx);
        p[i][1] = b.y + ((i & 2) ? b.hy : -b.hy);
        p[i][2] = b.z + ((i & 4) ? b.hz : -b.hz);
    }
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
            if (!(i & bit)) AddLine(out, p[i][0], p[i][1], p[i][2], p[i | bit][0], p[i | bit][1], p[i | bit][2], color);
}

void AddRing(std::vector<hud::Line>& out, const dm::Box& b, uint32_t color) {
    const float y = b.y - b.hy;  // on the ground under the box
    float px = b.x + b.ring, pz = b.z;
    for (int i = 1; i <= kRingSegments; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / kRingSegments;
        const float x = b.x + std::cos(t) * b.ring, z = b.z + std::sin(t) * b.ring;
        AddLine(out, px, y, pz, x, y, z, color);
        px = x, pz = z;
    }
}

}  // namespace

void Tick(bool want) {
    dm::SetWanted(want);
    if (!want) {
        if (g.on) {
            g.on = false;
            g.logged.clear();
            ClearAll();
            SR2_INFO("[debug_tools] highlight off");
        }
        return;
    }
    if (!g.on) {
        g.on = true;
        SR2_INFO("[debug_tools] highlight on (modules publish their boxes within a few seconds)");
    }
    const std::vector<dm::Category> cats = dm::Snapshot();
    // log what is highlighted when it changes (at most every kLogEveryMs)
    std::string summary;
    for (const dm::Category& c : cats) summary += std::format("{}{} {}", summary.empty() ? "" : ", ", c.boxes.size(), c.key);
    const unsigned long long now = GetTickCount64();
    if (summary != g.logged && now - g.lastLog >= kLogEveryMs) {
        g.logged = summary;
        g.lastLog = now;
        SR2_INFO("[debug_tools] highlight: {}", summary.empty() ? "nothing published yet" : summary);
    }
    float vp[16], eye[3];
    if (sr2::ui::overlay::IsOpen() || !hitboxes::CameraFrame(vp, eye)) return ClearAll();
    hud::Frame lines;
    std::copy(vp, vp + 16, lines.vp);
    std::set<std::string> used;
    size_t drawn = 0;
    for (const dm::Category& c : cats) {
        hud::Frame labels;
        std::copy(vp, vp + 16, labels.vp);
        labels.textOnly = true;
        labels.tag = c.label;
        labels.color = c.color;
        labels.range = kMaxLabelDistance;
        labels.showDistance = false;
        labels.size = 0.6f;
        for (const dm::Box& b : c.boxes) {
            if (drawn >= kMaxBoxes) break;
            const float dx = b.x - eye[0], dy = b.y - eye[1], dz = b.z - eye[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(d) || d > kMaxBoxDistance + b.ring) continue;
            if (d <= kMaxBoxDistance) {
                AddBox(lines.lines, b, c.color);
                ++drawn;
            }
            if (b.ring > 0) AddRing(lines.lines, b, c.color);
            if (d <= kMaxLabelDistance) labels.markers.push_back({b.x, b.y + b.hy + 0.35f, b.z, d});
        }
        const std::string owner = std::string(kLinesOwner) + "." + c.key;
        if (labels.markers.empty()) continue;
        hud::Publish(owner, std::move(labels));
        used.insert(owner);
    }
    for (const std::string& o : g.owners)
        if (!used.count(o)) hud::Clear(o);
    g.owners = std::move(used);
    if (lines.lines.empty()) hud::Clear(kLinesOwner);
    else hud::Publish(kLinesOwner, std::move(lines));
}

}  // namespace sr2::modules::debug_tools::highlight
