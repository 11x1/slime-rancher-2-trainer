#pragma once
// The overlay's Slime Rancher 2 look: two palettes (Dark, Light), the ImGui
// style metrics and the embedded fonts. Render thread only.

#include "imgui.h"

namespace sr2::ui::theme {

enum class Mode { Dark = 0, Light = 1 };

// Colours the custom widgets need besides the ImGuiStyle slots.
struct Palette {
    ImU32 win, side, card, cardBorder, line, frame, frameHover, hover, sel;
    ImU32 btn, btnHover, btnActive;
    ImU32 accent, accentInk, grab, grabActive;
    ImU32 tabSel, tabInk, head, offInk, offBg;
    ImU32 text, soft, dim, ok, warn, err;
    ImU32 scroll, shadow, scrim, pill, flap1, flap2;
    ImU32 tipBg, tipText, tipBorder;  // tooltips: must stand out from cards and the window
};

const Palette& Colors();  // palette of the applied mode
Mode Current();

// Applies the style for `mode` at `scale` (1 = 1080p) if either changed since
// the last call. Cheap to call every frame.
void Sync(Mode mode, float scale);

// Base sizes before scaling (style.FontScaleDpi applies the scale).
inline constexpr float kBodySize = 15.0f;
inline constexpr float kSmallSize = 13.0f;
inline constexpr float kDisplaySize = 16.0f;

// Loads the embedded body (Nunito Bold) and display (Fredoka SemiBold) fonts.
// `fallbackTtf` (e.g. Segoe UI) is merged into both for characters outside
// the embedded subset; nullptr or a missing file skips it. Call once, before
// the first frame.
void LoadFonts(ImGuiIO& io, const char* fallbackTtf);
ImFont* Body();     // default font
ImFont* Display();  // headings, sidebar, tabs (falls back to Body)

}  // namespace sr2::ui::theme
