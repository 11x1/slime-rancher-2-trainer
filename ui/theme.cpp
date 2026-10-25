// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "theme.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace sr2::ui::theme {
namespace {

#include "fonts/fredoka_semibold.inc"
#include "fonts/jetbrains_mono_medium.inc"
#include "fonts/nunito_bold.inc"

constexpr ImU32 Rgb(unsigned rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, static_cast<unsigned>(a * 255.0f + 0.5f));
}

// Night on the Rainbow Fields: plum panels, slime pink, sky-blue selection.
constexpr Palette kDark{
    .win = Rgb(0x241F3D), .side = Rgb(0x1C1832), .card = Rgb(0x2C2650), .cardBorder = Rgb(0x2C2650, 0.0f),
    .line = Rgb(0x463D78), .frame = Rgb(0x352E5C), .frameHover = Rgb(0x43397A), .hover = Rgb(0x2A2449),
    .sel = Rgb(0x4B4088),
    .btn = Rgb(0x4B4088), .btnHover = Rgb(0xD42A6B), .btnActive = Rgb(0xB71D58),
    .accent = Rgb(0xD42A6B), .accentInk = Rgb(0xFFFFFF), .grab = Rgb(0xFF8FC0), .grabActive = Rgb(0xFFB8D6),
    .tabSel = Rgb(0x92DFFC), .tabInk = Rgb(0x16213A), .head = Rgb(0x92DFFC), .offInk = Rgb(0x6F6894),
    .offBg = Rgb(0x3B3466),
    .text = Rgb(0xF4F1FF), .soft = Rgb(0xC9C3E8), .dim = Rgb(0x8F88B5), .ok = Rgb(0x8CEB7A), .warn = Rgb(0xFFCF5A),
    .err = Rgb(0xFF7A6B),
    .scroll = Rgb(0x4F4585), .shadow = Rgb(0x100C22, 0.42f), .scrim = Rgb(0x100C22, 0.60f),
    .pill = Rgb(0x241F3D, 0.86f), .flap1 = Rgb(0xFFECF6, 0.95f), .flap2 = Rgb(0xD6C4EE, 0.95f),
    .popBg = Rgb(0x3B336B), .popBorder = Rgb(0x574C93), .optHover = Rgb(0x554A99), .optSel = Rgb(0x483F82),
};

// Daytime: cream panels, white cards, the same pink and sky blue.
constexpr Palette kLight{
    .win = Rgb(0xFFF8EE), .side = Rgb(0xF6E8DC), .card = Rgb(0xFFFFFF), .cardBorder = Rgb(0xF2E1EA),
    .line = Rgb(0xEBCFDD), .frame = Rgb(0xF1E7FF), .frameHover = Rgb(0xE5D6FF), .hover = Rgb(0xF1E2EC),
    .sel = Rgb(0xE5D6FF),
    .btn = Rgb(0xC9EEFC), .btnHover = Rgb(0xFFC2DA), .btnActive = Rgb(0xFFADCD),
    .accent = Rgb(0xD42A6B), .accentInk = Rgb(0xFFFFFF), .grab = Rgb(0xD42A6B), .grabActive = Rgb(0x9E1A4C),
    .tabSel = Rgb(0x92DFFC), .tabInk = Rgb(0x16213A), .head = Rgb(0xB8205A), .offInk = Rgb(0xB2A9C8),
    .offBg = Rgb(0xEADFF0),
    .text = Rgb(0x2B2350), .soft = Rgb(0x4F4874), .dim = Rgb(0x8A82A6), .ok = Rgb(0x2E7D32), .warn = Rgb(0x9A6200),
    .err = Rgb(0xC0342F),
    .scroll = Rgb(0xDCCBEB), .shadow = Rgb(0x2B2350, 0.22f), .scrim = Rgb(0x2B2350, 0.35f),
    .pill = Rgb(0xFFF8EE, 0.94f), .flap1 = Rgb(0x3C326E, 0.95f), .flap2 = Rgb(0x241F3D, 0.95f),
    .popBg = Rgb(0xF1EAFE), .popBorder = Rgb(0xD9C8F5), .optHover = Rgb(0xDCCAFA), .optSel = Rgb(0xE7DCFC),
};

Mode g_mode = Mode::Dark;
float g_scale = 0.0f;  // 0 = never applied
ImFont* g_body = nullptr;
ImFont* g_display = nullptr;
ImFont* g_mono = nullptr;

ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void SetColors(ImGuiStyle& st, const Palette& p) {
    const ImVec4 none(0, 0, 0, 0);
    ImVec4* c = st.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = none;
    c[ImGuiCol_Text] = V(p.text);
    c[ImGuiCol_TextDisabled] = V(p.dim);
    c[ImGuiCol_WindowBg] = V(p.win);
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = V(p.card);
    c[ImGuiCol_Border] = V(p.line);
    c[ImGuiCol_FrameBg] = V(p.frame);
    c[ImGuiCol_FrameBgHovered] = V(p.frameHover);
    c[ImGuiCol_FrameBgActive] = V(p.frameHover);
    c[ImGuiCol_TitleBg] = V(p.card);
    c[ImGuiCol_TitleBgActive] = V(p.card);
    c[ImGuiCol_TitleBgCollapsed] = V(p.card);
    c[ImGuiCol_MenuBarBg] = V(p.side);
    c[ImGuiCol_ScrollbarGrab] = V(p.scroll);
    c[ImGuiCol_ScrollbarGrabHovered] = V(p.frameHover);
    c[ImGuiCol_ScrollbarGrabActive] = V(p.grab);
    c[ImGuiCol_CheckMark] = V(p.grab);
    c[ImGuiCol_SliderGrab] = V(p.grab);
    c[ImGuiCol_SliderGrabActive] = V(p.grabActive);
    c[ImGuiCol_Button] = V(p.btn);
    c[ImGuiCol_ButtonHovered] = V(p.btnHover);
    c[ImGuiCol_ButtonActive] = V(p.btnActive);
    c[ImGuiCol_Header] = V(p.sel);
    c[ImGuiCol_HeaderHovered] = V(p.sel);
    c[ImGuiCol_HeaderActive] = V(p.sel);
    c[ImGuiCol_Separator] = V(p.line);
    c[ImGuiCol_SeparatorHovered] = V(p.line);
    c[ImGuiCol_SeparatorActive] = V(p.line);
    c[ImGuiCol_Tab] = V(p.side);
    c[ImGuiCol_TabHovered] = V(p.hover);
    c[ImGuiCol_TabSelected] = V(p.tabSel);
    c[ImGuiCol_TextLink] = V(p.head);
    c[ImGuiCol_TextSelectedBg] = ImVec4(V(p.accent).x, V(p.accent).y, V(p.accent).z, 0.35f);
    c[ImGuiCol_InputTextCursor] = V(p.text);
    c[ImGuiCol_NavCursor] = V(p.tabSel);
    c[ImGuiCol_NavWindowingHighlight] = V(p.tabSel);
    c[ImGuiCol_NavWindowingDimBg] = V(p.scrim);
    c[ImGuiCol_ModalWindowDimBg] = V(p.scrim);
    c[ImGuiCol_PlotLines] = V(p.grab);
    c[ImGuiCol_PlotHistogram] = V(p.grab);
}

void SetMetrics(ImGuiStyle& st) {
    st.DisabledAlpha = 0.38f;
    st.WindowPadding = ImVec2(16, 14);
    st.WindowRounding = 18;
    st.WindowBorderSize = 2;
    st.ChildRounding = 14;
    st.ChildBorderSize = 1;
    st.PopupRounding = 12;
    st.PopupBorderSize = 1;
    st.FramePadding = ImVec2(10, 6.5f);  // 28 px tall frames with the 15 px body font
    st.FrameRounding = 10;
    st.FrameBorderSize = 0;
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.IndentSpacing = 18;
    st.ScrollbarSize = 12;
    st.ScrollbarRounding = 6;
    st.GrabMinSize = 12;
    st.GrabRounding = 7;
    st.TabRounding = 10;
    st.SelectableRounding = 8;  // dropdown options; no contiguous multi-selections in this UI
    st.MenuItemRounding = 8;    // context menu rows (MenuItem, BeginMenu), round 16
    st.SelectableTextAlign = ImVec2(0, 0.5f);
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    st.SeparatorTextBorderSize = 2;
}

}  // namespace

const Palette& Colors() { return g_mode == Mode::Light ? kLight : kDark; }
Mode Current() { return g_mode; }

void Sync(Mode mode, float scale) {
    if (g_scale > 0.0f && mode == g_mode && std::fabs(scale - g_scale) < 0.01f) return;
    g_mode = mode;
    g_scale = scale;
    ImGuiStyle st;
    SetMetrics(st);
    SetColors(st, Colors());
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
    st.FontSizeBase = kBodySize;
    ImGui::GetStyle() = st;
}

void LoadFonts(ImGuiIO& io, const char* fallbackTtf) {
    std::error_code ec;
    const bool haveFallback = fallbackTtf && *fallbackTtf && std::filesystem::exists(fallbackTtf, ec);
    auto merge = [&] {
        if (!haveFallback) return;
        ImFontConfig m;
        m.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(fallbackTtf, kBodySize, &m);
    };

    ImFontConfig body;
    strncpy_s(body.Name, "Nunito Bold", _TRUNCATE);
    g_body = io.Fonts->AddFontFromMemoryCompressedTTF(nunito_bold_compressed_data, static_cast<int>(nunito_bold_compressed_size),
                                                      kBodySize, &body);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    merge();

    ImFontConfig disp;
    strncpy_s(disp.Name, "Fredoka SemiBold", _TRUNCATE);
    g_display = io.Fonts->AddFontFromMemoryCompressedTTF(fredoka_semibold_compressed_data,
                                                         static_cast<int>(fredoka_semibold_compressed_size), kDisplaySize, &disp);
    if (g_display) merge();
    else g_display = g_body;
    // Inline code (console names): JetBrains Mono, ASCII only, uncompressed (9 KB). The atlas
    // only reads the data (FontDataOwnedByAtlas off), so the const array can be passed.
    ImFontConfig mono;
    strncpy_s(mono.Name, "JetBrains Mono Medium", _TRUNCATE);
    mono.FontDataOwnedByAtlas = false;
    g_mono = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(jetbrains_mono_medium_data),
                                            static_cast<int>(jetbrains_mono_medium_size), kSmallSize, &mono);
    io.FontDefault = g_body;
}

ImFont* Body() { return g_body; }
ImFont* Display() { return g_display ? g_display : g_body; }
ImFont* Mono() { return g_mono ? g_mono : g_body; }

}  // namespace sr2::ui::theme

#endif  // SR2_HAVE_IMGUI
