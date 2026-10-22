#include "overlay.h"

#if defined(SR2_HAVE_IMGUI)

#include <windows.h>
#include <shellapi.h>

#include <d3d11.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"

#include "core/hook_manager.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/perf.h"
#include "core/profiles.h"
#include "core/settings.h"
#include "core/version_guard.h"
#include "game_input.h"
#include "hud.h"
#include "momentum_hud.h"
#include "settings_view.h"

#ifndef SR2MODS_VERSION
#define SR2MODS_VERSION "dev"
#endif

// Provided by imgui_impl_win32.cpp (declared there behind #if 0 for users).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace sr2::ui {
namespace {

using core::HookManager;

// ---------------------------------------------------------------- hooks ----
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                    const UINT*, IUnknown* const*);

// vtable slots (fixed by the COM ABI of the DXGI interfaces).
constexpr size_t kSlotPresent = 8;          // IDXGISwapChain::Present
constexpr size_t kSlotResizeBuffers = 13;   // IDXGISwapChain::ResizeBuffers
constexpr size_t kSlotPresent1 = 22;        // IDXGISwapChain1::Present1
constexpr size_t kSlotResizeBuffers1 = 39;  // IDXGISwapChain3::ResizeBuffers1

PresentFn g_origPresent = nullptr;
Present1Fn g_origPresent1 = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
ResizeBuffers1Fn g_origResizeBuffers1 = nullptr;

// ------------------------------------------------------------ renderer ----
std::atomic<bool> g_failed{false};  // permanent: overlay disabled
bool g_initDone = false;            // render thread only
IDXGISwapChain* g_swapChain = nullptr;  // not AddRef'd: identity only
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_bbWidth = 0, g_bbHeight = 0;
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
std::mutex g_rtvMutex;  // Present vs ResizeBuffers (normally the same thread)
float g_appliedScale = 0.0f;
unsigned long long g_initTick = 0;
thread_local bool t_inHook = false;

// ---------------------------------------------------------------- state ----
std::atomic<bool> g_open{false};
std::atomic<int> g_captureState{0};  // 0 idle, 1 capturing
settings::Def g_captureDef;          // render thread only
std::bitset<256> g_captureSnapshot;
bool g_prevHotkeyDown = true;  // require a release first (key may be held at startup)
bool g_wasOpen = false;
bool g_softCursor = false;
bool g_textInputActive = false;  // ImGui text field had focus last frame (render thread)

// Profiles section (render thread only).
std::string g_profSel;       // selected profile in the combo
char g_profName[64] = {};    // "Save as" / "Rename" name field
char g_profDesc[256] = {};   // "Save as" description field
std::string g_profPopupMsg;  // error shown inside the open popup

// Window messages are queued on the window thread and replayed into ImGui on
// the render thread, so ImGui is only ever touched from one thread and the
// window thread never waits for a frame (no cross-thread deadlock).
struct QueuedMsg {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};
std::mutex g_msgMutex;
std::vector<QueuedMsg> g_msgQueue;
constexpr size_t kMaxQueued = 2048;

// ------------------------------------------------------------ helpers ----
void LogSeh(const char* where, DWORD code) {
    SR2_ERROR("ui: native exception 0x{:08X} in overlay {} - overlay disabled", code, where);
}

// Left/right click can never be the hotkey (every UI click would toggle).
int HotkeyVk() {
    int vk = static_cast<int>(std::lround(settings::Get(kUiModule, kKeyOverlayHotkey)));
    return (vk > VK_RBUTTON && vk < 255) ? vk : VK_F8;
}

// Mouse buttons 3-5 pressed since the last poll, as bits (1 << vk), latched by
// the WndProc from WM_MBUTTONDOWN / WM_XBUTTONDOWN. A click shorter than one
// frame can fall between two GetAsyncKeyState polls; the latch catches it.
std::atomic<unsigned> g_mouseLatch{0};

void LatchMouseDown(UINT msg, WPARAM wp) noexcept {
    int vk = 0;
    if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK) vk = VK_MBUTTON;
    else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK)
        vk = GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    if (vk) g_mouseLatch.fetch_or(1u << vk, std::memory_order_relaxed);
}

// Consume the latch bit for vk (false for anything but mouse 3-5).
bool TakeMouseLatch(int vk) {
    if (vk != VK_MBUTTON && vk != VK_XBUTTON1 && vk != VK_XBUTTON2) return false;
    const unsigned bit = 1u << vk;
    return (g_mouseLatch.fetch_and(~bit, std::memory_order_relaxed) & bit) != 0;
}

bool IsInputMsg(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUT || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE;
}

enum class WndAction { PassToGame, Swallow, DefaultProc };

// Mod logic of the WndProc detour (C++ part; exceptions never escape).
WndAction WndFilterCpp(UINT msg, WPARAM wp, LPARAM lp) noexcept {
    LatchMouseDown(msg, wp);  // open or not: the hotkey may be a mouse button
    const bool open = g_open.load(std::memory_order_relaxed);
    if (!open) return WndAction::PassToGame;
    const bool forward = IsInputMsg(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                         msg == WM_INPUTLANGCHANGE || msg == WM_DEVICECHANGE;
    if (forward && msg != WM_INPUT) {
        try {
            std::lock_guard lk(g_msgMutex);
            if (g_msgQueue.size() < kMaxQueued) g_msgQueue.push_back({msg, wp, lp});
        } catch (...) {
        }
    }
    if (!IsInputMsg(msg)) return WndAction::PassToGame;
    // Keep mouse/keyboard away from the game while the overlay is open.
    // System keys (Alt+F4, Alt+Enter) still reach the game's own handler
    // (gameplay actions are locked separately, see game_input.h); raw input
    // only gets the mandatory DefWindowProc cleanup.
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR) return WndAction::PassToGame;
    if (msg == WM_INPUT) return WndAction::DefaultProc;
    return WndAction::Swallow;
}

WndAction WndFilterGuarded(UINT msg, WPARAM wp, LPARAM lp) {
    __try {
        return WndFilterCpp(msg, wp, lp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
        g_open = false;
        LogSeh("WndProc", GetExceptionCode());
        return WndAction::PassToGame;
    }
}

// The game's own window procedure always runs unguarded; after a fault the
// detour is a pure pass-through.
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WndAction act = WndAction::PassToGame;
    if (!g_failed.load(std::memory_order_relaxed)) act = WndFilterGuarded(msg, wp, lp);
    switch (act) {
        case WndAction::Swallow: return 0;
        case WndAction::DefaultProc: return DefWindowProcW(hwnd, msg, wp, lp);
        default: return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }
}

void DrainMessages() {
    std::vector<QueuedMsg> msgs;
    {
        std::lock_guard lk(g_msgMutex);
        msgs.swap(g_msgQueue);
    }
    if (!g_open.load()) return;  // stale input from before closing
    for (const auto& m : msgs) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.wp, m.lp);
}

bool ForegroundIsGame() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Codes key capture accepts: every keyboard key plus mouse 3-5. Left/right
// click stay usable for the UI (the click that starts capture is one), the
// generic Shift/Ctrl/Alt codes are skipped in favour of their L/R variants,
// and IME/packet pseudo keys are not real keys.
bool IsCapturableVk(int vk) {
    switch (vk) {
        case VK_LBUTTON: case VK_RBUTTON: case 0x07:
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_PROCESSKEY: case VK_PACKET:
            return false;
        default: return vk > 0 && vk < 255;
    }
}

void EndCapture() {
    g_captureState = 0;
    g_mouseLatch = 0;         // a latched capture click must not toggle the overlay next
    g_prevHotkeyDown = true;  // the captured key may be the hotkey: wait for release
}

void StartCapture(const settings::Def& d) {
    g_captureDef = d;
    g_captureSnapshot.reset();
    for (int vk = 1; vk < 255; ++vk)
        if (KeyDown(vk)) g_captureSnapshot.set(static_cast<size_t>(vk));
    g_mouseLatch = 0;
    g_captureState = 1;
}

// Key capture by polling (independent of how the game consumes input), plus
// the WndProc latch for mouse clicks shorter than a frame. Takes the first
// newly pressed key: Esc cancels, Backspace/Delete clears the bind (0 =
// unbound; the overlay hotkey goes back to its default instead).
void PollCapture() {
    if (g_captureState.load() != 1) return;
    if (!ForegroundIsGame()) {
        g_mouseLatch = 0;
        return;
    }
    for (int vk = 1; vk < 255; ++vk) {
        if (!IsCapturableVk(vk)) continue;
        const bool latched = TakeMouseLatch(vk);
        const bool down = KeyDown(vk);
        const bool was = g_captureSnapshot.test(static_cast<size_t>(vk));
        g_captureSnapshot.set(static_cast<size_t>(vk), down);
        if (!latched && (!down || was)) continue;
        if (vk == VK_ESCAPE) {
            // cancelled
        } else if (vk == VK_BACK || vk == VK_DELETE) {
            const bool isHotkey = g_captureDef.module == kUiModule && g_captureDef.key == kKeyOverlayHotkey;
            const double v = isHotkey ? g_captureDef.def : 0.0;
            Write(g_captureDef, v);
            SR2_INFO("ui: {}.{} = {}", g_captureDef.module, g_captureDef.key, KeyName(static_cast<int>(v)));
        } else {
            Write(g_captureDef, static_cast<double>(vk));
            SR2_INFO("ui: {}.{} = {}", g_captureDef.module, g_captureDef.key, KeyName(vk));
        }
        EndCapture();
        return;
    }
}

// True if the key produces a character (letters, digits, punctuation, space).
bool IsCharacterKey(int vk) { return MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) != 0; }

void PollHotkey() {
    if (g_captureState.load() == 1) return;
    const int vk = HotkeyVk();
    const bool fg = ForegroundIsGame();
    // A mouse hotkey clicked and released within one frame only shows up in
    // the WndProc latch; treat it as a fresh press.
    // All bits are dropped each frame so a stale click can't fire after a rebind.
    const unsigned bits = g_mouseLatch.exchange(0, std::memory_order_relaxed);
    const bool isMouse = vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    const bool latched = fg && isMouse && (bits & (1u << vk)) != 0;
    const bool down = fg && KeyDown(vk);
    // While a text field has focus, a character hotkey is typed, not a toggle.
    const bool typing = g_textInputActive && g_open.load() && IsCharacterKey(vk);
    if ((latched || down) && !g_prevHotkeyDown && !typing) overlay::SetOpen(!g_open.load());
    g_prevHotkeyDown = down;
}

// Mouse buttons by polling as a fallback in case the game's window does not
// receive legacy WM_*BUTTON* messages (ImGui drops duplicate states).
void PollMouseButtons(ImGuiIO& io) {
    if (!ForegroundIsGame()) return;
    const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    io.AddMouseButtonEvent(0, KeyDown(swap ? VK_RBUTTON : VK_LBUTTON));
    io.AddMouseButtonEvent(1, KeyDown(swap ? VK_LBUTTON : VK_RBUTTON));
    io.AddMouseButtonEvent(2, KeyDown(VK_MBUTTON));
}

void ApplyScale(float clientHeight) {
    float s = std::clamp(clientHeight / 1080.0f, 1.0f, 3.0f);
    if (std::fabs(s - g_appliedScale) < 0.01f) return;
    g_appliedScale = s;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScaleAllSizes(s);
    style.FontScaleDpi = s;
}

void CreateRtv() {
    std::lock_guard lk(g_rtvMutex);
    if (g_rtv || !g_swapChain) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    g_bbWidth = td.Width;
    g_bbHeight = td.Height;
    HRESULT hr = g_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    if (FAILED(hr)) {
        g_rtv = nullptr;
        static int warned = 0;
        if (warned++ < 3) SR2_WARN("ui: CreateRenderTargetView failed (0x{:08X}, format {})", static_cast<unsigned>(hr),
                                   static_cast<int>(td.Format));
    }
}

void ReleaseRtv() {
    std::lock_guard lk(g_rtvMutex);
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool InitImGui(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {
        SR2_ERROR("ui: overlay: IDXGISwapChain::GetDesc failed or no output window");
        return false;
    }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) {
        SR2_ERROR("ui: overlay: the swap chain does not belong to a D3D11 device (renderer is not D3D11?)");
        return false;
    }
    g_device = dev;  // keep the reference for the lifetime of the process
    g_device->GetImmediateContext(&g_context);
    g_swapChain = sc;
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // never write imgui.ini into the game folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;

    // Prefer Segoe UI over the built-in bitmap-style font if available.
    char winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryA(winDir, MAX_PATH) > 0) {
        std::string font = std::string(winDir) + "\\Fonts\\segoeui.ttf";
        if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f);
    }

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        SR2_ERROR("ui: overlay: ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        SR2_ERROR("ui: overlay: ImGui_ImplDX11_Init failed");
        return false;
    }
    // IME positioning would call into the window from the render thread.
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    ApplyScale(static_cast<float>(rc.bottom - rc.top));

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
    if (!g_origWndProc)
        SR2_WARN("ui: overlay: SetWindowLongPtrW(GWLP_WNDPROC) failed ({}); game input is not blocked while open",
                 GetLastError());

    g_initTick = GetTickCount64();
    SR2_INFO("ui: overlay initialised on swap chain {} (hwnd {}, {}x{}, ImGui {}); toggle with {}",
             static_cast<void*>(sc), static_cast<void*>(g_hwnd), desc.BufferDesc.Width, desc.BufferDesc.Height,
             IMGUI_VERSION, KeyName(HotkeyVk()));
    SetOverlayStatus("active (" + KeyName(HotkeyVk()) + ")");
    return true;
}

// ------------------------------------------------------------------ UI ----
void HelpTooltip(const std::string& tip) {
    if (tip.empty()) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip.c_str());
}

void DrawDef(const settings::Def& d, bool moduleEnabled) {
    ImGui::PushID(d.key.c_str());
    const double cur = Read(d);
    ImGui::BeginDisabled(!moduleEnabled);
    switch (d.kind) {
        case settings::Kind::Bool: {
            bool v = cur >= 0.5;
            if (ImGui::Checkbox(d.label.c_str(), &v)) Write(d, v ? 1.0 : 0.0);
            break;
        }
        case settings::Kind::Float: {
            float v = static_cast<float>(cur);
            char fmt[16];
            std::snprintf(fmt, sizeof(fmt), "%%.%df", StepDecimals(d.step));
            if (ImGui::SliderFloat(d.label.c_str(), &v, static_cast<float>(d.min), static_cast<float>(d.max), fmt)) {
                double snapped = Snap(d, v);
                if (std::fabs(snapped - cur) > 1e-9) Write(d, snapped);
            }
            break;
        }
        case settings::Kind::Int: {
            int v = static_cast<int>(std::lround(cur));
            if (ImGui::SliderInt(d.label.c_str(), &v, static_cast<int>(std::lround(d.min)), static_cast<int>(std::lround(d.max)))) {
                double snapped = Snap(d, v);
                if (std::fabs(snapped - cur) > 1e-9) Write(d, snapped);
            }
            break;
        }
        case settings::Kind::Enum: {
            int idx = static_cast<int>(std::lround(cur));
            std::string preview = FormatValue(d, cur);
            if (ImGui::BeginCombo(d.label.c_str(), preview.c_str())) {
                for (int i = 0; i < static_cast<int>(d.options.size()); ++i) {
                    bool sel = i == idx;
                    if (ImGui::Selectable(d.options[static_cast<size_t>(i)].c_str(), sel)) Write(d, i);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case settings::Kind::Key: {
            const bool capturing = g_captureState.load() == 1 && g_captureDef.module == d.module && g_captureDef.key == d.key;
            std::string text = capturing ? std::string("Press a key or mouse 3/4/5...")
                                         : KeyName(static_cast<int>(std::lround(cur)));
            text += "###keybtn";
            if (ImGui::Button(text.c_str(), ImVec2(ImGui::CalcItemWidth(), 0)) && !capturing) StartCapture(d);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(capturing ? "Esc cancels, Backspace/Delete clears the bind."
                                            : "Click, then press a key or mouse button 3/4/5.");
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextUnformatted(d.label.c_str());
            break;
        }
    }
    ImGui::EndDisabled();
    HelpTooltip(d.tooltip);
    ImGui::PopID();
}

void DrawModule(const ModuleView& mv) {
    ImGui::PushID(mv.module.c_str());
    if (ImGui::CollapsingHeader(mv.title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        bool failed = false;
        std::string status = ModuleStatus(mv.module, &failed);
        if (mv.module == kUiModule) {
            ImGui::TextDisabled("Overlay: %s", OverlayStatus().c_str());
            ImGui::TextDisabled("Options menu tab: %s", OptionsTabStatus().c_str());
        } else if (!status.empty()) {
            if (failed)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Status: %s", status.c_str());
            else
                ImGui::TextDisabled("Status: %s", status.c_str());
        }

        bool moduleEnabled = true;
        if (mv.hasEnabled && mv.module != kUiModule) {
            bool en = Read(mv.enabled) >= 0.5;
            ImGui::PushID("enabled");
            if (ImGui::Checkbox(mv.enabled.label.empty() ? "Enabled" : mv.enabled.label.c_str(), &en))
                Write(mv.enabled, en ? 1.0 : 0.0);
            ImGui::PopID();
            HelpTooltip(mv.enabled.tooltip);
            moduleEnabled = Read(mv.enabled) >= 0.5;
        }

        for (const auto& g : mv.groups) {
            if (!g.name.empty()) ImGui::SeparatorText(g.name.c_str());
            ImGui::PushID(g.name.c_str());
            for (const auto& d : g.defs) DrawDef(d, moduleEnabled);
            ImGui::PopID();
        }

        if (ImGui::SmallButton("Reset to defaults")) {
            for (const auto& g : mv.groups)
                for (const auto& d : g.defs) Write(d, d.def);
        }
        ImGui::Unindent();
        ImGui::Spacing();
    }
    ImGui::PopID();
}

// ------------------------------------------------------------ profiles ----
namespace profiles = ::sr2::core::profiles;

bool SameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
}

const profiles::Info* FindProfile(const std::vector<profiles::Info>& list, const std::string& name) {
    for (const auto& i : list)
        if (SameName(i.name, name)) return &i;
    return nullptr;
}

// Name fields only accept what SanitizeName keeps.
int ProfileNameFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                    c == '_';
    return ok ? 0 : 1;
}

// Explorer on the profiles folder. ShellExecuteW runs on a short-lived thread,
// so a slow shell never stalls the game's render thread (shell32 is
// delay-loaded). Falls back to starting explorer.exe directly.
DWORD WINAPI OpenFolderThread(LPVOID p) {
    std::wstring* dir = static_cast<std::wstring*>(p);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", dir->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        std::wstring cmd = L"explorer.exe \"" + *dir + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            SR2_WARN("ui: cannot open the profiles folder (ShellExecute {}, CreateProcess error {})",
                     reinterpret_cast<INT_PTR>(r), GetLastError());
        }
    }
    delete dir;
    return 0;
}

void OpenProfilesFolder() {
    profiles::Init();
    auto* dir = new std::wstring(profiles::Folder().wstring());
    if (HANDLE t = CreateThread(nullptr, 0, &OpenFolderThread, dir, 0, nullptr))
        CloseHandle(t);
    else
        delete dir;
}

void CopyToBuf(char* buf, size_t size, const std::string& s) {
    size_t n = std::min(s.size(), size - 1);
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
}

bool PopupCancelled() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false); }

const ImVec4 kColWarn(1.0f, 0.8f, 0.2f, 1.0f);
const ImVec4 kColErr(1.0f, 0.45f, 0.35f, 1.0f);

void DrawProfiles() {
    if (!ImGui::CollapsingHeader("Profiles", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("profiles");
    ImGui::Indent();

    const auto list = profiles::List();
    const std::string active = profiles::ActiveProfile();
    const profiles::Info* activeInfo = active.empty() ? nullptr : FindProfile(list, active);
    const bool modified = activeInfo && profiles::IsModified();

    if (active.empty())
        ImGui::TextDisabled("Active profile: none");
    else if (!activeInfo)
        ImGui::TextColored(kColWarn, "Active profile: %s (file missing)", active.c_str());
    else if (modified)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Active profile: %s (modified)", activeInfo->name.c_str());
    else
        ImGui::Text("Active profile: %s", activeInfo->name.c_str());

    // Keep the selection on an existing profile (default: the active one).
    if (const profiles::Info* found = FindProfile(list, g_profSel))
        g_profSel = found->name;
    else
        g_profSel = activeInfo ? activeInfo->name : (list.empty() ? std::string() : list.front().name);
    const profiles::Info* sel = FindProfile(list, g_profSel);

    auto labelOf = [&](const profiles::Info& i) {
        std::string l = i.name;
        if (activeInfo && SameName(i.name, activeInfo->name)) l += modified ? "  [active, modified]" : "  [active]";
        if (!i.valid) l += "  [broken]";
        return l;
    };

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    const std::string preview = sel ? labelOf(*sel) : std::string("(no profiles)");
    if (ImGui::BeginCombo("##profile", preview.c_str())) {
        for (const auto& i : list) {
            const bool isSel = sel && SameName(i.name, sel->name);
            std::string item = labelOf(i) + "##" + i.name;
            if (ImGui::Selectable(item.c_str(), isSel)) g_profSel = i.name;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                if (!i.valid)
                    ImGui::SetTooltip("Broken file: %s", i.error.c_str());
                else if (!i.description.empty())
                    ImGui::SetTooltip("%s", i.description.c_str());
            }
            if (isSel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel || !sel->valid);
    if (ImGui::Button("Load") && sel) profiles::Load(sel->name);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Apply every value of the selected profile (values it lacks reset to their defaults).");

    if (sel) {
        ImGui::PushTextWrapPos(0.0f);
        if (!sel->valid)
            ImGui::TextColored(kColErr, "Broken file: %s", sel->error.c_str());
        else if (!sel->description.empty())
            ImGui::TextDisabled("%s", sel->description.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("No profiles yet - use \"Save as...\" to create one.");
    }

    ImGui::BeginDisabled(active.empty());
    if (ImGui::Button("Save")) ImGui::OpenPopup("Save profile?");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Overwrite the active profile with the current settings.");
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        g_profName[0] = 0;
        g_profDesc[0] = 0;
        g_profPopupMsg.clear();
        ImGui::OpenPopup("Save profile as");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Rename") && sel) {
        CopyToBuf(g_profName, sizeof(g_profName), profiles::SanitizeName(sel->name));
        g_profPopupMsg.clear();
        ImGui::OpenPopup("Rename profile");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && sel) ImGui::OpenPopup("Delete profile?");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open profiles folder")) OpenProfilesFolder();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Profiles are .json files: share one by copying its file. Put shared files in this folder;\n"
                          "the list refreshes every time the overlay opens.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) profiles::Refresh();

    unsigned long long tick = 0;
    const std::string status = profiles::Status(&tick);
    if (!status.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (GetTickCount64() - tick < 4000)
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", status.c_str());
        else
            ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }

    // ---- popups (same ID stack as the OpenPopup calls above) ----
    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    const float fieldW = 320.0f * (g_appliedScale > 0 ? g_appliedScale : 1.0f);

    if (ImGui::BeginPopupModal("Save profile?", nullptr, popupFlags)) {
        ImGui::Text("Overwrite profile '%s' with the current settings?", active.c_str());
        if (ImGui::Button("Save")) {
            if (!active.empty())
                profiles::SaveCurrentAs(active, activeInfo ? activeInfo->description : std::string(), true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Save profile as", nullptr, popupFlags)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldW);
        const bool enter = ImGui::InputText("Name", g_profName, sizeof(g_profName),
                                            ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                            &ProfileNameFilter);
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("Description (optional)", g_profDesc, sizeof(g_profDesc));
        const std::string name = profiles::SanitizeName(g_profName);
        const profiles::Info* existing = name.empty() ? nullptr : FindProfile(list, name);
        if (name.empty())
            ImGui::TextDisabled("Letters, digits, space, - and _ (max 40).");
        else if (existing)
            ImGui::TextColored(kColWarn, "'%s' exists and will be overwritten.", existing->name.c_str());
        else
            ImGui::TextDisabled("File: profiles\\%s.json", name.c_str());
        if (!g_profPopupMsg.empty()) ImGui::TextColored(kColErr, "%s", g_profPopupMsg.c_str());
        ImGui::BeginDisabled(name.empty());
        // Replacing a file needs the explicit "Overwrite" click; Enter never confirms it.
        const bool doSave = ImGui::Button(existing ? "Overwrite" : "Save") || (enter && !existing && !name.empty());
        ImGui::EndDisabled();
        if (doSave) {
            auto r = profiles::SaveCurrentAs(name, g_profDesc, existing != nullptr);
            if (r.ok()) {
                g_profSel = r.name;
                ImGui::CloseCurrentPopup();
            } else {
                g_profPopupMsg = r.message;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Rename profile", nullptr, popupFlags)) {
        ImGui::Text("Rename '%s' to:", g_profSel.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldW);
        const bool enter = ImGui::InputText("##newname", g_profName, sizeof(g_profName),
                                            ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                            &ProfileNameFilter);
        const std::string name = profiles::SanitizeName(g_profName);
        const profiles::Info* existing = name.empty() ? nullptr : FindProfile(list, name);
        const bool clash = existing && !SameName(existing->name, g_profSel);
        if (name.empty())
            ImGui::TextDisabled("Letters, digits, space, - and _ (max 40).");
        else if (clash)
            ImGui::TextColored(kColWarn, "'%s' exists and will be overwritten.", existing->name.c_str());
        if (!g_profPopupMsg.empty()) ImGui::TextColored(kColErr, "%s", g_profPopupMsg.c_str());
        ImGui::BeginDisabled(name.empty() || g_profSel.empty());
        const bool doRename = ImGui::Button(clash ? "Overwrite" : "Rename") || (enter && !clash && !name.empty());
        ImGui::EndDisabled();
        if (doRename && !g_profSel.empty()) {
            auto r = profiles::Rename(g_profSel, name, clash);
            if (r.ok()) {
                g_profSel = r.name;
                ImGui::CloseCurrentPopup();
            } else {
                g_profPopupMsg = r.message;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete profile?", nullptr, popupFlags)) {
        ImGui::Text("Delete profile '%s'? Its file is removed.", g_profSel.c_str());
        if (ImGui::Button("Delete")) {
            if (!g_profSel.empty()) profiles::Delete(g_profSel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::PopID();
}

void DrawOverlayWindow() {
    const float s = g_appliedScale > 0 ? g_appliedScale : 1.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * s, 620.0f * s), ImGuiCond_FirstUseEver);
    bool open = true;
    std::string title = "sr2mods " SR2MODS_VERSION " - mod settings###sr2mods";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("%s closes this window. Changes apply immediately and are saved to sr2mods.json.",
                            KeyName(HotkeyVk()).c_str());
        if (core::version_guard::Mismatch()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Version warning: %s",
                               core::version_guard::Status().c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "This game build differs from the one the mod was made for; some modules may be disabled.");
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextDisabled("%s", core::version_guard::Status().c_str());
        }
        ImGui::Separator();

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("##settings", ImVec2(0, -footer))) {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            DrawProfiles();
            auto views = BuildModuleViews();
            if (views.empty()) ImGui::TextDisabled("No settings registered.");
            for (const auto& mv : views) DrawModule(mv);
            ImGui::PopItemWidth();
        }
        ImGui::EndChild();

        ImGui::Separator();
        unsigned long long last = LastWriteTick();
        if (last == 0)
            ImGui::TextDisabled("No changes");
        else if (GetTickCount64() - last < 1500)
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "Saving...");
        else
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Saved");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Close").x - ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::Button("Close")) open = false;
    }
    ImGui::End();
    if (!open) overlay::SetOpen(false);
}

void DrawHint() {
    const float s = g_appliedScale > 0 ? g_appliedScale : 1.0f;
    ImGui::SetNextWindowPos(ImVec2(12.0f * s, 12.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##sr2mods_hint", nullptr, f)) {
        ImGui::Text("sr2mods: press %s for mod settings", KeyName(HotkeyVk()).c_str());
        if (core::version_guard::Mismatch())
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Version warning: %s", core::version_guard::Status().c_str());
    }
    ImGui::End();
}

bool HintVisible() {
    if (g_initTick == 0) return false;
    if (GetTickCount64() - g_initTick > 10000) return false;
    return settings::GetBool(kUiModule, kKeyOverlayHint);
}

void RenderFrame() {
    PollHotkey();
    PollCapture();

    const bool open = g_open.load();
    if (g_wasOpen && !open) {
        g_captureState = 0;
        FlushNow();
    }
    if (!g_wasOpen && open) profiles::Refresh();  // users may have dropped shared files in
    g_wasOpen = open;

    const bool hint = !open && HintVisible();
    const bool hudMarkers = hud::HasContent();  // world markers published by modules (wall_markers)
    const bool momentum = momentum_hud::HasContent();  // hl2_movement speed readout
    if (!open && !hint && !hudMarkers && !momentum) {
        std::lock_guard lk(g_msgMutex);
        g_msgQueue.clear();
        return;
    }

    if (!g_rtv) CreateRtv();
    if (!g_rtv) return;

    ImGuiIO& io = ImGui::GetIO();
    DrainMessages();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // The back buffer can differ from the client area (scaled fullscreen);
    // ImGui works in client coordinates, the renderer maps them onto the buffer.
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && g_bbWidth && g_bbHeight)
        io.DisplayFramebufferScale = ImVec2(g_bbWidth / io.DisplaySize.x, g_bbHeight / io.DisplaySize.y);
    ApplyScale(io.DisplaySize.y);

    if (open) {
        PollMouseButtons(io);
        io.MouseDrawCursor = !core::main_thread::Ready() || game_input::WantSoftwareCursor();
    } else {
        io.MouseDrawCursor = false;
    }

    ImGui::NewFrame();
    if (hudMarkers) hud::Draw();  // foreground draw list (the module hides them while the settings window is open)
    if (momentum) momentum_hud::Draw();
    if (open)
        DrawOverlayWindow();
    else if (hint)
        DrawHint();
    ImGui::Render();
    g_textInputActive = io.WantTextInput;

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (auto* r : oldRtv)
        if (r) r->Release();
    if (oldDsv) oldDsv->Release();
}

void OnPresentImpl(IDXGISwapChain* sc) {
    if (!g_initDone) {
        g_initDone = true;
        if (!InitImGui(sc)) {
            g_failed = true;
            SetOverlayStatus("failed to initialise (see log)");
            return;
        }
    }
    if (sc != g_swapChain) return;  // some other swap chain (not the game window)
    RenderFrame();
}

void OnPresentGuarded(IDXGISwapChain* sc) {
    __try {
        OnPresentImpl(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
        g_open = false;  // stop swallowing input immediately
        LogSeh("Present", GetExceptionCode());
    }
}

void OnPresent(IDXGISwapChain* sc, UINT flags) {
    if (g_failed.load(std::memory_order_relaxed) || t_inHook || (flags & DXGI_PRESENT_TEST)) return;
    t_inHook = true;
    {
        SR2_PERF_SCOPE("hook:ui.Present");  // the overlay's own work (render thread)
        OnPresentGuarded(sc);
    }
    t_inHook = false;
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    OnPresent(sc, flags);
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    OnPresent(sc, flags);
    return g_origPresent1(sc, sync, flags, p);
}

// Releasing our back-buffer RTV must happen even after a fault: DXGI fails
// ResizeBuffers while any back-buffer reference is alive. It only touches our
// own COM object, never game code.
void ReleaseRtvGuarded(IDXGISwapChain* sc) {
    __try {
        if (sc && sc == g_swapChain) ReleaseRtv();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
        g_open = false;
        LogSeh("ResizeBuffers", GetExceptionCode());
    }
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    ReleaseRtvGuarded(sc);
    return g_origResizeBuffers(sc, n, w, h, f, fl);
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                           const UINT* mask, IUnknown* const* queues) {
    ReleaseRtvGuarded(static_cast<IDXGISwapChain*>(sc));
    return g_origResizeBuffers1(sc, n, w, h, f, fl, mask, queues);
}

// ------------------------------------------------------- vtable lookup ----
struct SwapChainVtbl {
    void* present = nullptr;
    void* resizeBuffers = nullptr;
    void* present1 = nullptr;
    void* resizeBuffers1 = nullptr;
};

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool ReadSwapChainVtable(SwapChainVtbl& out, std::string& err) {
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d11) {
        err = "d3d11.dll not available";
        return false;
    }
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) {
        err = "D3D11CreateDeviceAndSwapChain not exported";
        return false;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"sr2mods_dxgi_probe";
    ATOM cls = RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"sr2mods", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                inst, nullptr);
    if (!hwnd) {
        err = "CreateWindowExW failed (" + std::to_string(GetLastError()) + ")";
        if (cls) UnregisterClassW(wc.lpszClassName, inst);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = create(nullptr, type, nullptr, 0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &sd, &sc,
                    &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *reinterpret_cast<void***>(sc);
        out.present = vt[kSlotPresent];
        out.resizeBuffers = vt[kSlotResizeBuffers];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
            out.present1 = (*reinterpret_cast<void***>(sc1))[kSlotPresent1];
            sc1->Release();
        }
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3) {
            out.resizeBuffers1 = (*reinterpret_cast<void***>(sc3))[kSlotResizeBuffers1];
            sc3->Release();
        }
        ok = out.present && out.resizeBuffers;
        if (!ok) err = "null vtable entries";
    } else {
        err = "D3D11CreateDeviceAndSwapChain failed (0x" + std::format("{:08X}", static_cast<unsigned>(hr)) + ")";
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return ok;
}

bool CreateAndEnable(const char* name, void* target, void* detour, void** orig, bool required) {
    std::string err;
    auto& hm = HookManager::Get();
    if (!hm.Create(kUiModule, name, target, detour, orig, &err) || !hm.Enable(name, &err)) {
        if (required)
            SR2_ERROR("ui: overlay: hook {} failed: {}", name, err);
        else
            SR2_WARN("ui: overlay: optional hook {} failed: {}", name, err);
        hm.Remove(name);
        *orig = nullptr;
        return false;
    }
    SR2_DEBUG("ui: hook {} at {}", name, target);
    return true;
}

void OnMainThreadTick(float /*dt*/) { game_input::Tick(g_open.load() && !g_failed.load()); }

}  // namespace

namespace overlay {

bool IsOpen() { return g_open.load(); }

void SetOpen(bool open) {
    if (g_failed.load()) open = false;
    if (g_open.exchange(open) != open) SR2_DEBUG("ui: overlay {}", open ? "opened" : "closed");
}

}  // namespace overlay

bool StartOverlay() {
    static bool started = false;
    if (started) return true;
    RegisterUiSettings();
    // Modules are set up by now: create the profiles folder / starter profiles.
    profiles::Init();
    SetOverlayStatus("starting");

    SwapChainVtbl vt;
    std::string err;
    if (!ReadSwapChainVtable(vt, err)) {
        SR2_ERROR("ui: overlay unavailable: cannot read the DXGI swap chain vtable ({})", err);
        SetOverlayStatus("unavailable: " + err);
        return false;
    }
    SR2_INFO("ui: DXGI vtable: Present {} ResizeBuffers {} Present1 {} ResizeBuffers1 {}", vt.present, vt.resizeBuffers,
             vt.present1, vt.resizeBuffers1);

    if (!HookManager::Get().Initialize()) {
        SR2_ERROR("ui: overlay unavailable: hook manager not initialised");
        SetOverlayStatus("unavailable: hook manager");
        return false;
    }
    // ResizeBuffers first, so the RTV is always released before a resize.
    if (!CreateAndEnable("ui.dxgi_resize_buffers", vt.resizeBuffers, reinterpret_cast<void*>(&HkResizeBuffers),
                         reinterpret_cast<void**>(&g_origResizeBuffers), true)) {
        SetOverlayStatus("unavailable: ResizeBuffers hook failed");
        return false;
    }
    if (vt.resizeBuffers1)
        CreateAndEnable("ui.dxgi_resize_buffers1", vt.resizeBuffers1, reinterpret_cast<void*>(&HkResizeBuffers1),
                        reinterpret_cast<void**>(&g_origResizeBuffers1), false);
    bool anyPresent = CreateAndEnable("ui.dxgi_present", vt.present, reinterpret_cast<void*>(&HkPresent),
                                      reinterpret_cast<void**>(&g_origPresent), true);
    if (vt.present1 && vt.present1 != vt.present)
        anyPresent |= CreateAndEnable("ui.dxgi_present1", vt.present1, reinterpret_cast<void*>(&HkPresent1),
                                      reinterpret_cast<void**>(&g_origPresent1), false);
    if (!anyPresent) {
        HookManager::Get().Remove("ui.dxgi_resize_buffers");
        HookManager::Get().Remove("ui.dxgi_resize_buffers1");
        SetOverlayStatus("unavailable: Present hook failed");
        return false;
    }

    if (core::main_thread::AddFrameCallback("ui.overlay", &OnMainThreadTick) == 0)
        SR2_WARN("ui: overlay: no main-thread callback; gameplay input is not locked while the overlay is open");

    started = true;
    SetOverlayStatus("waiting for first frame");
    SR2_INFO("ui: overlay hooks installed; press {} in game", KeyName(HotkeyVk()));
    return true;
}

}  // namespace sr2::ui

#else  // !SR2_HAVE_IMGUI

#include "core/log.h"
#include "settings_view.h"

namespace sr2::ui {
namespace overlay {
bool IsOpen() { return false; }
void SetOpen(bool) {}
}  // namespace overlay
bool StartOverlay() {
    SR2_ERROR("ui: overlay unavailable: built without Dear ImGui (third_party/imgui missing)");
    SetOverlayStatus("unavailable: built without Dear ImGui");
    return false;
}
}  // namespace sr2::ui

#endif  // SR2_HAVE_IMGUI
