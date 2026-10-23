#pragma once
// HUD edit mode: move and resize HUD elements (minimap, Speed Panel) with the
// mouse. The "Move" button on an element's overlay page starts it; the
// overlay's settings window closes, the element gets a dashed outline and the
// overlay keeps mouse and keyboard (game camera and actions stay locked, like
// while the settings window is open).
//
//   drag inside       move; snaps to the horizontal screen centre (dashed guide)
//   drag a corner     resize, the opposite corner stays put; Alt: from the centre
//   Shift (moving)    the gap on the axis you drag along becomes equal to the
//                     other gap of the nearest corner (equal distance to both edges)
//   Enter / click outside   keep;  Esc   cancel (restore)
//
// Placement: x, y in percent of the free space (0 = left / top edge, 100 =
// right / bottom edge: screen size minus element size), size in percent of
// the screen height. Resolution independent.
//
// Threading: Begin / Draw / Cancel run on the render thread (overlay and
// menu). Preview / Active / Editing may be called from any thread (the
// minimap applies the preview on the Unity main thread). Commits are written
// through settings_view's Write (posted to the main thread).

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
// True once after edit mode ended (keep or cancel): the overlay reopens its window.
bool TakeFinished();

}  // namespace sr2::ui::hud_edit
