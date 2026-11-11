#pragma once

#include "pm4.h"

#include <cstdint>
#include <vector>

namespace ps4::gnm {

// Executes one submitted frame's draws against a D3D11 swapchain attached to
// `hwnd` (an HWND passed as void* so callers do not pull in <windows.h>).
void RenderFrame(void* hwnd, const std::vector<Draw>& draws);

// Re-presents the latest rendered frame with live overlay/HUD updates without
// advancing guest simulation (used when presentation FPS > 60 Hz or in menus).
void PresentCachedFrame(void* hwnd);

// Forwards Win32 window messages to the native overlay & display manager.
// Returns true when the overlay consumed the input event.
bool OnWindowMessage(void* hwnd, uint32_t msg, uint64_t wp, int64_t lp);

// True while the native Settings / Online overlay is open and capturing input.
bool IsOverlayCapturingInput();

// Applies display mode (Windowed / Borderless / Exclusive Fullscreen) & size.
void ApplyDisplaySettings(void* hwnd);

// Releases D3D11 swapchain/fullscreen state cleanly before exit.
void ShutdownRenderer();

}  // namespace ps4::gnm
