#pragma once
// HUD edit mode: move and resize HUD elements (minimap, Speed Panel) with the
// mouse. The "Move" button on an element's overlay page starts it, and so does
// dragging the element itself while the overlay's settings window is open
// (Hover). The settings window closes, the element gets a dashed outline, a
// help bar with cancel / save buttons sits at the top centre, and the overlay
// keeps mouse and keyboard (game camera and actions stay locked, like while
// the settings window is open).
//
//   drag inside       move; snaps to the horizontal screen centre (dashed guide)
//   drag a corner     resize, the opposite corner stays put; Alt: from the centre
//   Shift (moving)    the gap on the axis you drag along becomes equal to the
//                     other gap of the nearest corner (equal distance to both edges)
//   Enter / click outside / check button   keep;  Esc / X button   cancel (restore)
//
// Placement: x, y in percent of the free space (0 = left / top edge, 100 =
// right / bottom edge: screen size minus element size), size in percent of
// the screen height. Resolution independent.
//
// Threading: Begin / Draw / Cancel run on the render thread (overlay and
// menu). Preview / Active / Editing may be called from any thread (the
// minimap applies the preview on the Unity main thread). Commits are written
// through settings_view's Write (posted to the main thread).

struct ImDrawList;

namespace sr2::ui::hud_edit {

enum class Element { Minimap, SpeedPanel };

struct Placement {
    float x = 0, y = 0;  // % of the free space
    float size = 0;      // % of the screen height (element-specific meaning)
};

// Top-left corner in px for a box of w x h on a W x H screen, and back.
inline float ToPx(float pct, float screen, float box) {
    const float freeSpace = screen > box ? screen - box : 0.0f;
    return freeSpace * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0f;
}
inline float ToPct(float px, float screen, float box) {
    const float freeSpace = screen - box;
    if (freeSpace <= 0.5f) return 50.0f;
    const float p = px / freeSpace * 100.0f;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

// Any thread. True while `e` is being edited, or after a commit until the
// new values have reached the settings (at most ~1.5 s): `out` is then the
// placement to show instead of the stored one.
bool Preview(Element e, Placement& out);
// Any thread. Edit mode is on (the overlay owns mouse and keyboard).
bool Active();
bool Editing(Element e);

// Overlay page (render thread): the element whose position/size settings
// module.key belongs to. MoveRow: the row that becomes the "Move" button (x);
// Hidden: the other placement keys (y, size), not drawn.
bool MoveRow(const char* module, const char* key, Element& out);
bool Hidden(const char* module, const char* key);
const char* Name(Element e);  // "minimap", "Speed Panel"

// Render thread.
void Begin(Element e);   // enter edit mode (the overlay closes its window)
void Cancel();           // restore and leave
void Draw(float uiScale);  // between NewFrame and Render, after the HUD content
// While the settings window is open (not in edit mode), before the window is
// drawn: an element under the mouse (outside every overlay window) gets a
// hover outline, and dragging it starts edit mode with that drag. `allowed`:
// the overlay can give the mouse to it (no key capture running).
void Hover(float uiScale, bool allowed);
// The help bar's keycap (monospace key name on a rounded chip with a darker
// edge below), for key hints elsewhere in the overlay. Render thread, ImGui
// builds only. `x` = left end, `cy` = vertical centre; returns the width.
// `alpha` fades the whole chip (lighter hints).
float DrawKeycap(ImDrawList* dl, float x, float cy, const char* key, float uiScale, float alpha = 1.0f);
float KeycapWidth(const char* key, float uiScale);
// Any thread. The last Hover had an element under the mouse or a press on one:
// the overlay keeps the mouse from the game (press, drag, release).
bool WantsMouse();
// True once after edit mode ended (keep or cancel): the overlay reopens its window.
bool TakeFinished();

}  // namespace sr2::ui::hud_edit
