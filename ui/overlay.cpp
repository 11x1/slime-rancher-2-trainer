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
#include "imgui_internal.h"

#include "core/guard.h"
#include "core/hook_manager.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/perf.h"
#include "core/profiles.h"
#include "core/settings.h"
#include "core/version_guard.h"
#include "autotest_view.h"
#include "game_input.h"
#include "hud.h"
#include "hud_edit.h"
#include "menu.h"
#include "momentum_hud.h"
#include "quick_start_hint.h"
#include "settings_view.h"
#include "slime_icons.h"
#include "theme.h"

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
// Copy of the back buffer for the theme peel (menu::WantSnapshot), render thread only.
ID3D11Texture2D* g_snapTex = nullptr;
ID3D11ShaderResourceView* g_snapSrv = nullptr;
D3D11_TEXTURE2D_DESC g_snapDesc{};
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
std::atomic<bool> g_typing{false};  // the same, for game_input on the main thread
// io.WantCaptureMouse of the last overlay frame (render thread writes, window
// thread reads). While open and false (pointer outside the overlay's windows,
// no ImGui drag, no modal), mouse input goes on to the game, so the game's own
// menus beside the overlay (Slimepedia, Options) stay clickable. Gameplay
// actions stay locked through the Input System map (game_input). true until
// the first frame after opening has computed it.
std::atomic<bool> g_imguiWantsMouse{true};
// Mouse buttons whose press went to the game: their release goes there too,
// wherever it happens (window thread only).
unsigned g_gameButtons = 0;
int g_openFrames = 0;  // overlay frames rendered since opening, capped at 2 (render thread)


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
// The overlay owns mouse and keyboard: settings window open, or HUD edit mode.
bool Captured() {
    return !g_failed.load(std::memory_order_relaxed) && (g_open.load(std::memory_order_relaxed) || hud_edit::Active());
}

// What the overlay's last-resort SEH guards caught: code, faulting instruction and, for an
// access violation, the address touched, so a fault can be traced to the code (with the PDB).
struct SehInfo {
    DWORD code = 0;
    void* addr = nullptr;
    ULONG_PTR avOp = 0, avAddr = 0;  // EXCEPTION_ACCESS_VIOLATION: 0 read, 1 write, 8 execute; target
    bool av = false;
};

int SehFilter(EXCEPTION_POINTERS* ep, SehInfo* out) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    out->code = r->ExceptionCode;
    out->addr = r->ExceptionAddress;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        out->av = true;
        out->avOp = r->ExceptionInformation[0];
        out->avAddr = r->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void LogSeh(const char* where, const SehInfo& i) {
    std::string at = core::DescribeAddress(i.addr);
    if (i.av) at += std::format(" ({} 0x{:X})", i.avOp == 1 ? "writing" : i.avOp == 8 ? "executing" : "reading", i.avAddr);
    SR2_ERROR("ui: native exception 0x{:08X} in overlay {} at {} - overlay disabled", i.code, where, at);
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

bool IsMouseMsg(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE;
}

// Bit for the button a WM_*BUTTON* message is about (0 for other messages).
unsigned ButtonBit(UINT msg, WPARAM wp, bool& down) {
    down = false;
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = true; [[fallthrough]];
        case WM_LBUTTONUP: return 1u;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = true; [[fallthrough]];
        case WM_RBUTTONUP: return 2u;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = true; [[fallthrough]];
        case WM_MBUTTONUP: return 4u;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: down = true; [[fallthrough]];
        case WM_XBUTTONUP: return GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 16u : 8u;
        default: return 0u;
    }
}

// Mouse message while the overlay is open: true = let the game have it.
bool MouseToGame(UINT msg, WPARAM wp) {
    bool down = false;
    const unsigned bit = ButtonBit(msg, wp, down);
    if (bit && !down && (g_gameButtons & bit)) {  // release of a press the game got
        g_gameButtons &= ~bit;
        return true;
    }
    // Key capture takes mouse 3-5 too: nothing goes to the game meanwhile.
    if (g_imguiWantsMouse.load(std::memory_order_relaxed) || g_captureState.load(std::memory_order_relaxed)) return false;
    if (bit && down) g_gameButtons |= bit;
    return true;
}

// Raw input from the mouse (not the keyboard): the header only, the game can
// still read the whole packet afterwards.
bool RawInputIsMouse(LPARAM lp) {
    RAWINPUTHEADER hdr{};
    UINT size = sizeof(hdr);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) ==
        static_cast<UINT>(-1))
        return false;
    return hdr.dwType == RIM_TYPEMOUSE;
}

// Alt+Space opens the overlay's search box (spotlight.cpp), but normally it opens the window's
// system menu: DefWindowProc turns the WM_SYSCHAR ' ' into WM_SYSCOMMAND SC_KEYMENU. While the
// settings window is open, the Space key messages with Alt and every SC_KEYMENU are kept from the
// game's window procedure (the key messages are still queued for ImGui). Any SC_KEYMENU, because
// the game sees the Alt press but not the Space, so the Alt release would enter menu mode.
bool IsSystemMenuKey(UINT msg, WPARAM wp) {
    if (!g_open.load(std::memory_order_relaxed)) return false;
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return wp == VK_SPACE;
    if (msg == WM_SYSCHAR) return wp == L' ';
    if (msg == WM_SYSCOMMAND) return (wp & 0xFFF0) == SC_KEYMENU;
    return false;
}

// Mod logic of the WndProc detour (C++ part; exceptions never escape).
WndAction WndFilterCpp(UINT msg, WPARAM wp, LPARAM lp) noexcept {
    LatchMouseDown(msg, wp);  // open or not: the hotkey may be a mouse button
    if (!Captured()) {
        g_gameButtons = 0;
        // A test question (autotest_view.h) is open: ImGui gets the input too, and the game loses only what
        // the panel uses (mouse over the panel, keys while its note field types).
        if (autotest::WantsInput() && (IsInputMsg(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS) && msg != WM_INPUT) {
            try {
                std::lock_guard lk(g_msgMutex);
                if (g_msgQueue.size() < kMaxQueued) g_msgQueue.push_back({msg, wp, lp});
            } catch (...) {
            }
            if (IsMouseMsg(msg) && msg != WM_MOUSEMOVE && autotest::PointerOnPanel()) return WndAction::Swallow;
            if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && autotest::Typing())
                return WndAction::Swallow;
        }
        return WndAction::PassToGame;
    }
    const bool forward = IsInputMsg(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                         msg == WM_INPUTLANGCHANGE || msg == WM_DEVICECHANGE;
    if (forward && msg != WM_INPUT) {
        try {
            std::lock_guard lk(g_msgMutex);
            if (g_msgQueue.size() < kMaxQueued) g_msgQueue.push_back({msg, wp, lp});
        } catch (...) {
        }
    }
    if (IsSystemMenuKey(msg, wp)) return WndAction::Swallow;
    if (!IsInputMsg(msg)) return WndAction::PassToGame;
    // Keep the keyboard, and the mouse over the overlay, away from the game
    // while the overlay is open. The mouse outside the overlay's windows goes
    // on to the game (MouseToGame), so its menus stay usable next to it.
    // System keys (Alt+F4, Alt+Enter) still reach the game's own handler
    // (gameplay actions are locked separately, see game_input.h); other raw
    // input only gets the mandatory DefWindowProc cleanup.
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR) return WndAction::PassToGame;
    if (msg == WM_INPUT) {
        const bool mouseFree =
            !g_imguiWantsMouse.load(std::memory_order_relaxed) && !g_captureState.load(std::memory_order_relaxed);
        if ((g_gameButtons || mouseFree) && RawInputIsMouse(lp))
            return WndAction::PassToGame;
        return WndAction::DefaultProc;
    }
    if (IsMouseMsg(msg) && MouseToGame(msg, wp)) return WndAction::PassToGame;
    return WndAction::Swallow;
}

WndAction WndFilterGuarded(UINT msg, WPARAM wp, LPARAM lp) {
    SehInfo seh;
    __try {
        return WndFilterCpp(msg, wp, lp);
    } __except (SehFilter(GetExceptionInformation(), &seh)) {
        g_failed = true;
        g_open = false;
        LogSeh("WndProc", seh);
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
    if (!Captured() && !autotest::WantsInput()) return;  // stale input from before closing
    for (const auto& m : msgs) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.wp, m.lp);
}

bool ForegroundIsGame() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---------------------------------------------------- Steam overlay key ----
// While the overlay is open, Shift+Tab must not open the Steam overlay. How GameOverlayRenderer64 sees
// its hotkey (offline analysis, ui/NOTES.md "Blocking game input" item 6): every frame it polls the
// real GetAsyncKeyState(VK_TAB) and, on each change, posts a private message to the game window
// (kSteamTabMsg, wParam kSteamTabMagic, lParam 1 = down / 0 = up). Its message handler (its own
// WH_GETMESSAGE hook plus Get/Peek/DispatchMessage detours) turns that message into a synthetic Tab
// key event for its key map and opens the overlay when the map holds Shift + Tab. Real Tab
// WM_KEYDOWNs never reach that map: with Shift held Steam only eats them (so the game, and ImGui,
// never see Shift+Tab). Round 9 filtered the real Tab key-down only, which is why it failed.
// So, while the overlay has the input, a WH_GETMESSAGE hook of ours on the window thread (newest on
// the thread, so it runs before Steam's hook, and before its Get/PeekMessage detours look at the
// message, which they do after the original returns):
//   - turns Steam's "Tab down" message into WM_NULL: Steam's map never gets Tab down. "Tab up" passes
//     (it can only clear the map, and a press from before opening must be able to end);
//   - queues each real Tab WM_KEYDOWN for ImGui and turns it into WM_NULL, so Tab / Shift+Tab reach
//     the overlay even though Steam would eat Shift+Tab (the window procedure never gets it twice).
// It is re-installed every couple of seconds while open, in case Steam re-installed its own hook
// (its "hook may have died" check) and got in front again.
constexpr UINT kSteamTabMsg = 0x14FC;          // GameOverlayRenderer64 RVA 0xADF10 posts it, 0xAC2B0 handles it
constexpr WPARAM kSteamTabMagic = 0x198239AC;  // its wParam
constexpr ULONGLONG kTabHookRefreshMs = 2000;
HHOOK g_tabHook = nullptr;    // render thread
bool g_tabWanted = false;     // last state passed on (render thread)
ULONGLONG g_tabHookTick = 0;  // when g_tabHook was installed (render thread)
std::atomic<unsigned> g_tabHookCalls{0}, g_steamTabDropped{0}, g_tabToImGui{0};  // since the menu opened

LRESULT CALLBACK SteamTabHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp && Captured()) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        g_tabHookCalls.fetch_add(1, std::memory_order_relaxed);
        if (m->message == kSteamTabMsg && m->wParam == kSteamTabMagic && m->lParam == 1) {
            if (wp == PM_REMOVE) g_steamTabDropped.fetch_add(1, std::memory_order_relaxed);
            m->message = WM_NULL;
        } else if (m->message == WM_KEYDOWN && m->wParam == VK_TAB) {
            if (wp == PM_REMOVE && m->hwnd == g_hwnd) {
                try {
                    std::lock_guard lk(g_msgMutex);
                    if (g_msgQueue.size() < kMaxQueued) g_msgQueue.push_back({m->message, m->wParam, m->lParam});
                } catch (...) {
                }
                g_tabToImGui.fetch_add(1, std::memory_order_relaxed);
            }
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Installs a fresh hook (the new one goes in before the old one is removed, so there is no gap).
void InstallTabHook() {
    static bool failLogged = false, okLogged = false;
    g_tabHookTick = GetTickCount64();  // a failure is retried at the next refresh, not every frame
    const DWORD tid = g_hwnd ? GetWindowThreadProcessId(g_hwnd, nullptr) : 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SteamTabHook), &self);
    HHOOK h = tid ? SetWindowsHookExW(WH_GETMESSAGE, &SteamTabHook, self, tid) : nullptr;
    if (!h) {
        if (!failLogged) {
            failLogged = true;
            SR2_WARN("ui: Shift+Tab block unavailable (SetWindowsHookEx WH_GETMESSAGE failed, thread {}, error {})", tid,
                     GetLastError());
        }
        return;
    }
    if (g_tabHook) UnhookWindowsHookEx(g_tabHook);
    g_tabHook = h;
    if (!okLogged) {
        okLogged = true;
        SR2_INFO("ui: Shift+Tab block: WH_GETMESSAGE hook on window thread {} (hwnd {}, render thread {}); Steam overlay "
                 "renderer {}",
                 tid, static_cast<void*>(g_hwnd), GetCurrentThreadId(),
                 GetModuleHandleW(L"GameOverlayRenderer64.dll") ? "loaded" : "not loaded");
    }
}

// Render thread, every frame: installs the hook when the overlay takes the input, refreshes it while
// the overlay keeps the input and removes it when the overlay gives the input back.
void SyncSteamKeyBlock(bool captured) {
    if (captured == g_tabWanted) {
        if (captured && GetTickCount64() - g_tabHookTick >= kTabHookRefreshMs) InstallTabHook();
        return;
    }
    g_tabWanted = captured;
    if (captured) {
        g_tabHookCalls = 0;
        g_steamTabDropped = 0;
        g_tabToImGui = 0;
        InstallTabHook();
        return;
    }
    if (!g_tabHook) return;
    UnhookWindowsHookEx(g_tabHook);
    g_tabHook = nullptr;
    const unsigned dropped = g_steamTabDropped.load(), toMenu = g_tabToImGui.load(), calls = g_tabHookCalls.load();
    if (dropped || toMenu)
        SR2_INFO("ui: Shift+Tab block: menu closed; dropped {} Steam Tab-down message(s), passed {} Tab press(es) to "
                 "the menu ({} hook calls)",
                 dropped, toMenu, calls);
    else if (!calls)
        SR2_WARN("ui: Shift+Tab block: the hook was never called while the menu was open (window thread {})",
                 GetWindowThreadProcessId(g_hwnd, nullptr));
}

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
    if ((latched || down) && !g_prevHotkeyDown && !typing) {
        if (hud_edit::Active()) hud_edit::Cancel();  // back to the settings window (TakeFinished reopens it)
        else overlay::SetOpen(!g_open.load());
    }
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

// Style for the current height and the Dark/Light setting (theme.cpp).
void ApplyScale(float clientHeight) {
    g_appliedScale = std::clamp(clientHeight / 1080.0f, 1.0f, 3.0f);
    menu::SyncTheme(g_appliedScale);
}

// The frame just rendered (old theme) as a texture for the theme peel.
// Invalid if the copy is not possible (multisampled back buffer, no memory).
ImTextureID SnapshotBackBuffer() {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return ImTextureID_Invalid;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1) {
        bb->Release();
        return ImTextureID_Invalid;
    }
    if (!g_snapTex || d.Width != g_snapDesc.Width || d.Height != g_snapDesc.Height || d.Format != g_snapDesc.Format) {
        if (g_snapSrv) g_snapSrv->Release();
        if (g_snapTex) g_snapTex->Release();
        g_snapSrv = nullptr;
        g_snapTex = nullptr;
        D3D11_TEXTURE2D_DESC nd = d;
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        nd.CPUAccessFlags = 0;
        nd.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                   : d.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateTexture2D(&nd, nullptr, &g_snapTex)) ||
            FAILED(g_device->CreateShaderResourceView(g_snapTex, &sd, &g_snapSrv))) {
            if (g_snapTex) g_snapTex->Release();
            g_snapTex = nullptr;
            g_snapSrv = nullptr;
            bb->Release();
            return ImTextureID_Invalid;
        }
        g_snapDesc = d;
    }
    g_context->CopyResource(g_snapTex, bb);
    bb->Release();
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(g_snapSrv));
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

    // Embedded Nunito / Fredoka; Segoe UI fills in characters outside their subset.
    char winDir[MAX_PATH] = {};
    std::string fallback;
    if (GetWindowsDirectoryA(winDir, MAX_PATH) > 0) fallback = std::string(winDir) + "\\Fonts\\segoeui.ttf";
    theme::LoadFonts(io, fallback.c_str());

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
    slime_icons::Init(g_device);

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
namespace profiles = ::sr2::core::profiles;

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

bool IsCapturing(const settings::Def& d) {
    return g_captureState.load() == 1 && g_captureDef.module == d.module && g_captureDef.key == d.key;
}

menu::Host MakeHost() {
    menu::Host h;
    h.scale = g_appliedScale > 0 ? g_appliedScale : 1.0f;
    h.nowMs = GetTickCount64();
    h.hotkeyName = KeyName(HotkeyVk());
    h.versionMismatch = core::version_guard::Mismatch();
    h.versionStatus = core::version_guard::Status();
    h.versionKnown = !h.versionMismatch && h.versionStatus.find("matches known build") != std::string::npos;
    h.startCapture = &StartCapture;
    h.isCapturing = &IsCapturing;
    h.openProfilesFolder = &OpenProfilesFolder;
    return h;
}

bool HintVisible() {
    if (g_initTick == 0) return false;
    if (GetTickCount64() - g_initTick > 10000) return false;
    return settings::GetBool(kUiModule, kKeyOverlayHint);
}

void RenderFrame() {
    PollHotkey();
    PollCapture();
    if (hud_edit::TakeFinished()) overlay::SetOpen(true);  // edit mode kept or cancelled: back to the page

    const bool open = g_open.load();
    const bool editing = hud_edit::Active();
    SyncSteamKeyBlock(Captured());
    if (!open) {
        g_openFrames = 0;
        g_imguiWantsMouse = true;
    }
    if (g_wasOpen && !open) {
        g_captureState = 0;
        FlushNow();
    }
    if (!g_wasOpen && open) {
        profiles::Refresh();  // users may have dropped shared files in
        menu::OnOpened();     // next slime for the logo
    }
    g_wasOpen = open;

    const bool menuHint = !open && HintVisible();
    const quick_start_hint::View quick = open ? quick_start_hint::View{} : quick_start_hint::Get();
    const bool hint = menuHint || quick.phase != quick_start_hint::Phase::Hidden;
    const bool hudMarkers = hud::HasContent();  // world markers published by modules (wall_markers)
    const bool momentum = momentum_hud::HasContent();  // hl2_movement speed readout
    const bool harness = autotest::HasContent();        // test harness tag / question (modules/autotest)
    if (!open && !editing && !hint && !hudMarkers && !momentum && !harness) {
        std::lock_guard lk(g_msgMutex);
        g_msgQueue.clear();
        return;
    }

    if (!g_rtv) CreateRtv();
    if (!g_rtv) return;

    ImGuiIO& io = ImGui::GetIO();
    {
        SR2_PERF_DETAIL("hook:ui.Present.imgui");  // CPU: input, ImGui frame, draw lists
        DrainMessages();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // The back buffer can differ from the client area (scaled fullscreen);
        // ImGui works in client coordinates, the renderer maps them onto the buffer.
        if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && g_bbWidth && g_bbHeight)
            io.DisplayFramebufferScale = ImVec2(g_bbWidth / io.DisplaySize.x, g_bbHeight / io.DisplaySize.y);
        ApplyScale(io.DisplaySize.y);

        if (open || editing || autotest::WantsInput()) PollMouseButtons(io);
        if (open || editing) {
            // The Win32 backend reads Shift / Ctrl with GetKeyState, whose state is per
            // thread and is not kept for this render thread (the window's messages are
            // read on the game's thread), so io.KeyShift / KeyCtrl could be stale until
            // some other input refreshed them (Sandbox grids: Shift+wheel did nothing
            // until the grid was clicked). The async state is the real one.
            // The backend also queues its stale GetKeyState modifiers with every key message (Shift
            // held: WM_KEYDOWN repeats ~30 times a second, each queueing "Shift up"). With ours after
            // them that toggled the modifier twice per frame, and the input queue's trickling spread the
            // toggles over the next frames, so io.KeyShift flickered and a Shift+wheel notch often came
            // in a frame with Shift "up" (it scrolled instead). Its modifier events are dropped here and
            // only the async state is queued.
            if (ForegroundIsGame()) {
                ImVector<ImGuiInputEvent>& q = ImGui::GetCurrentContext()->InputEventsQueue;
                for (int i = q.Size - 1; i >= 0; --i)
                    if (q[i].Type == ImGuiInputEventType_Key &&
                        (q[i].Key.Key == ImGuiMod_Shift || q[i].Key.Key == ImGuiMod_Ctrl || q[i].Key.Key == ImGuiMod_Alt))
                        q.erase(q.Data + i);
                io.AddKeyEvent(ImGuiMod_Shift, KeyDown(VK_SHIFT));
                io.AddKeyEvent(ImGuiMod_Ctrl, KeyDown(VK_CONTROL));
                io.AddKeyEvent(ImGuiMod_Alt, KeyDown(VK_MENU));
            }
            io.MouseDrawCursor = !core::main_thread::Ready() || game_input::WantSoftwareCursor();
        } else {
            io.MouseDrawCursor = false;
        }

        ImGui::NewFrame();
        if (open) {
            // NewFrame computed it from the windows drawn last frame: only valid
            // once the menu has been drawn at least once since opening.
            if (g_openFrames < 2) ++g_openFrames;
            // A HUD element under the mouse (hud_edit::Hover) keeps the press and drag from the game too.
            g_imguiWantsMouse = g_openFrames < 2 || io.WantCaptureMouse || hud_edit::WantsMouse();
        }
        if (hudMarkers) hud::Draw();  // foreground draw list (the module hides them while the settings window is open)
        if (momentum) momentum_hud::Draw();
        if (editing) hud_edit::Draw(g_appliedScale);  // outline, handles, guide line (mouse and keys are ours)
        if (open) {
            // Drag a HUD element: edit mode. Not while a game menu (map, pause, inventory) covers the HUD.
            if (!editing) hud_edit::Hover(g_appliedScale, g_captureState.load() == 0 && !game_input::GameMenuOpen());
            if (!menu::Draw(MakeHost())) overlay::SetOpen(false);
            if (hud_edit::Active()) overlay::SetOpen(false);  // "Move" clicked or element dragged: edit mode
        } else if (hint && !editing) {
            menu::Host h = MakeHost();
            h.menuHint = menuHint;
            h.quickHint = static_cast<int>(quick.phase);  // Hidden 0, Pending 1, Staying 2
            h.quickSeconds = quick.secondsLeft;
            menu::DrawHint(h);
        }
        if (harness) autotest::Draw(g_appliedScale, ForegroundIsGame(), g_textInputActive && open);  // on top of the menu
        ImGui::Render();
        g_textInputActive = io.WantTextInput;
        g_typing.store(g_textInputActive, std::memory_order_relaxed);
    }

    // D3D11 submission on the game's immediate context: a driver / GPU stall shows up here.
    SR2_PERF_DETAIL("hook:ui.Present.d3d");

    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Theme switched this frame: keep it (still the old look) for the peel.
    if (menu::WantSnapshot()) menu::StartPeel(SnapshotBackBuffer());
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (auto* r : oldRtv)
        if (r) r->Release();
    if (oldDsv) oldDsv->Release();
}

// ---------------------------------------------------------- screenshots ----
#if SR2MODS_AUTOTEST
// The test harness (autotest_view.h) asked for the back buffer: copied through a staging texture
// (resolved first when multisampled) and converted to RGBA8 here; the PNG is encoded off this thread.
float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = mant ? 0.0f : 65504.0f;
    else v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

unsigned char LinearToSrgb8(float v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(std::lround(s * 255.0f));
}

autotest::Shot CaptureBackBuffer() {
    autotest::Shot shot;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) {
        shot.error = "IDXGISwapChain::GetBuffer failed";
        return shot;
    }
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    DXGI_FORMAT fmt = d.Format;
    switch (fmt) {  // typeless back buffers: read as the plain format
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: fmt = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: fmt = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: fmt = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        default: break;
    }
    ID3D11Texture2D* src = bb;
    ID3D11Texture2D* resolved = nullptr;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = d;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = 0;
        rd.CPUAccessFlags = 0;
        rd.MiscFlags = 0;
        rd.MipLevels = 1;
        rd.ArraySize = 1;
        if (FAILED(g_device->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) {
            bb->Release();
            shot.error = "cannot resolve the multisampled back buffer";
            return shot;
        }
        g_context->ResolveSubresource(resolved, 0, bb, 0, fmt);
        src = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
        if (resolved) resolved->Release();
        bb->Release();
        shot.error = "cannot create a staging texture";
        return shot;
    }
    g_context->CopyResource(staging, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned w = d.Width, h = d.Height;
        shot.width = w;
        shot.height = h;
        shot.rgba.resize(static_cast<size_t>(w) * h * 4);
        bool known = true;
        for (unsigned y = 0; y < h && known; ++y) {
            const auto* row = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
            unsigned char* out = shot.rgba.data() + static_cast<size_t>(y) * w * 4;
            for (unsigned x = 0; x < w && known; ++x, out += 4) {
                switch (fmt) {
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                        out[0] = row[x * 4 + 0];
                        out[1] = row[x * 4 + 1];
                        out[2] = row[x * 4 + 2];
                        break;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    case DXGI_FORMAT_B8G8R8X8_UNORM:
                    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                        out[0] = row[x * 4 + 2];
                        out[1] = row[x * 4 + 1];
                        out[2] = row[x * 4 + 0];
                        break;
                    case DXGI_FORMAT_R10G10B10A2_UNORM: {
                        uint32_t p;
                        std::memcpy(&p, row + x * 4, 4);
                        out[0] = static_cast<unsigned char>((p & 0x3FF) >> 2);
                        out[1] = static_cast<unsigned char>(((p >> 10) & 0x3FF) >> 2);
                        out[2] = static_cast<unsigned char>(((p >> 20) & 0x3FF) >> 2);
                        break;
                    }
                    case DXGI_FORMAT_R16G16B16A16_FLOAT: {  // scRGB (linear): encoded as sRGB, clipped
                        uint16_t c[3];
                        std::memcpy(c, row + static_cast<size_t>(x) * 8, 6);
                        out[0] = LinearToSrgb8(HalfToFloat(c[0]));
                        out[1] = LinearToSrgb8(HalfToFloat(c[1]));
                        out[2] = LinearToSrgb8(HalfToFloat(c[2]));
                        break;
                    }
                    default: known = false; break;
                }
                out[3] = 255;
            }
        }
        g_context->Unmap(staging, 0);
        if (known) {
            shot.ok = true;
        } else {
            shot.rgba.clear();
            shot.error = std::format("unsupported back buffer format {}", static_cast<int>(d.Format));
        }
    } else {
        shot.error = "cannot map the staging texture";
    }
    staging->Release();
    if (resolved) resolved->Release();
    bb->Release();
    return shot;
}
#endif  // SR2MODS_AUTOTEST

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
#if SR2MODS_AUTOTEST
    if (autotest::ShotWanted(false)) autotest::DeliverShot(CaptureBackBuffer());  // the game's frame only
    RenderFrame();
    if (autotest::ShotWanted(true)) autotest::DeliverShot(CaptureBackBuffer());  // with the overlay
#else
    RenderFrame();
#endif
}

void OnPresentGuarded(IDXGISwapChain* sc) {
    SehInfo seh;
    __try {
        OnPresentImpl(sc);
    } __except (SehFilter(GetExceptionInformation(), &seh)) {
        g_failed = true;
        g_open = false;  // stop swallowing input immediately
        LogSeh("Present", seh);
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
    SehInfo seh;
    __try {
        if (sc && sc == g_swapChain) ReleaseRtv();
    } __except (SehFilter(GetExceptionInformation(), &seh)) {
        g_failed = true;
        g_open = false;
        LogSeh("ResizeBuffers", seh);
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

// The pointer over the overlay (or a text field in use) also keeps the game's menu input
// (full map, pause menu) and its UI pointer away from the game (game_input.h).
// A test question's panel under the pointer, or its note field typing, locks the game's input the same
// way (autotest_view.h), also while the overlay is closed.
void OnMainThreadTick(float /*dt*/) {
    const bool captured = Captured() && !g_failed.load();
    const bool panel = !g_failed.load() && autotest::WantsGameInputLock();
    game_input::Tick(captured || panel,
                     (captured && g_imguiWantsMouse.load(std::memory_order_relaxed)) || (panel && autotest::PointerOnPanel()),
                     (captured && g_typing.load(std::memory_order_relaxed)) || (panel && autotest::Typing()));
}

}  // namespace

namespace overlay {

bool IsOpen() { return Captured(); }

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

// IM_ASSERT (third_party/imgui/imconfig.h): ImGui's checks, logged once per check
// (file and line) instead of compiled out, so a misuse names itself in the log. ImGui
// carries on as it would without asserts. Render thread (ImGui is used nowhere else).
void Sr2ImGuiAssertFailed(const char* expr, const char* file, int line) {
    std::string where = file ? file : "?";
    if (const size_t slash = where.find_last_of("\\/"); slash != std::string::npos) where.erase(0, slash + 1);
    where += ":" + std::to_string(line);
    static std::mutex mutex;
    static std::vector<std::string> seen;
    {
        std::lock_guard lk(mutex);
        if (seen.size() >= 64 || std::find(seen.begin(), seen.end(), where) != seen.end()) return;
        seen.push_back(where);
    }
    // The callers as module+offset (look them up with the build's PDB): the check names the
    // ImGui line, the stack the overlay code that got there.
    void* frames[10] = {};
    const USHORT n = CaptureStackBackTrace(1, 10, frames, nullptr);
    std::string stack;
    for (USHORT i = 0; i < n; ++i) stack += (i ? " < " : "") + sr2::core::DescribeAddress(frames[i]);
    SR2_ERROR("ui: ImGui check failed: {} ({}) - logged once; called from {}", expr ? expr : "?", where, stack);
}

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
