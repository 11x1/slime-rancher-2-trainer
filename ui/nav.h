#pragma once
// Keyboard focus for the overlay's own drawn widgets (switches, tabs, tiles, list rows).
//
// ImGui's InvisibleButton takes no keyboard focus, so Tab skipped every widget the menu draws itself
// (round 10: "checkboxes dont get focused"). nav::Button is an InvisibleButton that takes Tab and
// arrow focus (Space / Enter press it); ImGui's own focus rectangle is not drawn under it, the caller
// draws Ring() after its own drawing, shaped like the widget. While the keyboard moves the focus,
// IsItemHovered() is true for the focused item, so hover looks and tooltips follow the focus.
//
// TabGroup makes a grid or list one Tab stop: Tab lands on the item focused last (else the first),
// Tab / Shift+Tab leave the group, the arrow keys move item by item inside it. The group's items must
// be in the same window as the items around them for Tab to leave it (a grid's scrolling child gets
// ImGuiChildFlags_NavFlattened).
//
// Render thread only (ImGui).

#include <cstdint>

#include "imgui.h"

namespace sr2::ui::nav {

// InvisibleButton that takes keyboard focus. No ImGui focus rectangle: draw Ring() after the widget.
bool Button(const char* id, ImVec2 size, ImGuiButtonFlags flags = 0);

// The last item has the keyboard focus and the focus cursor is showing (keyboard in use).
bool Focused();

// The focus outline (ImGui's nav cursor colour and thickness, a few px outside [a, b]) around the last
// item if Focused(). `rounding` is the widget's own corner radius.
void Ring(float rounding);
void Ring(ImVec2 a, ImVec2 b, float rounding);

// The context-menu key (Menu, or Shift+F10) was pressed while the last item has the keyboard focus:
// open its right-click menu.
bool ContextKey();

// Pushes ImGuiItemFlags_NoTabStop for the items until the matching PopNoTabStop: reachable with the
// arrow keys, skipped by Tab (buttons inside a list row whose row is the Tab stop).
void PushNoTabStop();
void PopNoTabStop();

class TabGroup {
public:
    explicit TabGroup(const char* id);  // before the first item, in the window that holds the group
    TabGroup(const TabGroup&) = delete;
    TabGroup& operator=(const TabGroup&) = delete;
    ~TabGroup();                        // = End() if not called

    void Item(uint64_t key);  // right before the item's widget; `key` identifies it across frames
    void Done();              // right after the item's widget
    void End();               // after the last item

private:
    ImGuiStorage* st_ = nullptr;
    ImGuiID id_ = 0;
    ImGuiID cur_ = 0;    // the Tab stop (hashed key); 0 = the first item
    ImGuiID first_ = 0;  // first item this frame
    ImGuiID item_ = 0;   // the item between Item() and Done()
    bool pushed_ = false, seen_ = false, ended_ = false;
};

}  // namespace sr2::ui::nav
