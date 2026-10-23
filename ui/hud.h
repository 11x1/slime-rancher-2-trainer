#pragma once
// Screen-space world markers drawn by the ImGui overlay (foreground draw list),
// also while the settings window is closed.
//
// Threading: a module publishes one Frame per owner from the Unity main thread
// (any thread is fine); the overlay reads the latest frames on the render thread
// inside its Present hook. Frames are copied under a mutex, nothing is shared
// by reference, and a frame older than kHudStaleMs is not drawn (main thread
// stalled, loading screen, module stopped publishing).
//
// Projection happens on the render thread with the main thread's view-projection
// matrix of the same frame: world -> clip = vp * (x, y, z, 1), Unity / OpenGL
// convention (clip.w > 0 in front of the camera, NDC y up).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr2::ui::hud {

inline constexpr unsigned long long kHudStaleMs = 250;

struct Marker {
    float x = 0, y = 0, z = 0;  // world position
    float distance = 0;         // metres (label + fade)
};

struct Frame {
    float vp[16] = {};          // column-major (Unity Matrix4x4 memory order): projection * worldToCamera
    std::vector<Marker> markers;
    float range = 60;           // markers fade towards this distance
    float size = 1;             // user scale
    bool showDistance = true;
    uint32_t color = 0xFFFF66E0;  // ImGui IM_COL32 order (A B G R in memory: 0xAABBGGRR)
    std::string tag;            // short text above the marker (optional)
    bool textOnly = false;      // draw only `tag` (no diamond), centred on the marker: a world-space message
};

// Replaces `owner`'s frame (stamped with the current tick).
void Publish(std::string_view owner, Frame frame);
// Removes `owner`'s frame (nothing drawn for it).
void Clear(std::string_view owner);
// True if any non-stale frame with markers exists (the overlay renders then).
bool HasContent();

// Render thread, between ImGui::NewFrame and ImGui::Render (overlay only).
void Draw();

}  // namespace sr2::ui::hud
