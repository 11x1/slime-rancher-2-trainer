#pragma once
// Dear ImGui settings overlay on the game's D3D11 swap chain.
//
// Hooks (MinHook via core::HookManager, module "ui"):
//   ui.dxgi_present         IDXGISwapChain::Present          (vtable slot 8)
//   ui.dxgi_present1        IDXGISwapChain1::Present1        (slot 22, if available)
//   ui.dxgi_resize_buffers  IDXGISwapChain::ResizeBuffers    (slot 13)
//   ui.dxgi_resize_buffers1 IDXGISwapChain3::ResizeBuffers1  (slot 39, if available)
// The vtable is read from a throw-away D3D11 device + swap chain created on a
// hidden window. ImGui is initialised on the first Present of the real swap
// chain; the game window's WndProc is subclassed (SetWindowLongPtrW) to feed
// ImGui and to keep input away from the game while the overlay is open.

#include "ui.h"

namespace sr2::ui::overlay {

// True while the overlay owns mouse and keyboard: the settings window is open
// or the HUD edit mode (hud_edit.h) is on.
bool IsOpen();
void SetOpen(bool open);

}  // namespace sr2::ui::overlay
