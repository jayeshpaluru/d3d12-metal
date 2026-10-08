// Operating system services the front-end needs but cannot get portably.
//
// The front-end builds natively on macOS (headless tests) and, later, as PE
// DLLs under Wine. The native build has no Win32 events, so it ships a minimal
// event object; the PE build maps the same calls onto the real Win32 API.
#pragma once

#include "common/com.h"
#include "common/export.h"

namespace d3d12m {

// Signals an event handle passed in by the application
// (ID3D12Fence::SetEventOnCompletion). Win32 builds call SetEvent.
void platform_set_event(HANDLE event);

// Windows and events for swap chains. The headless native build has neither:
// it reports no window and creates no events.

// Client area size in pixels. False if `window` is not a window.
bool platform_window_client_size(HWND window, UINT *width, UINT *height);
// The top-level window containing `window` (itself if it is one).
HWND platform_root_window(HWND window);
// An auto-reset event, or null.
HANDLE platform_create_event(bool signaled);
// A new handle to the same event, or null.
HANDLE platform_duplicate_event(HANDLE event);
void platform_close_event(HANDLE event);

} // namespace d3d12m

// Native-build event objects, exported for the tests. They stand in for
// CreateEvent / WaitForSingleObject / CloseHandle. A successful wait consumes
// the signal (auto-reset).
D3D12M_EXPORT HANDLE d3d12metal_native_create_event();
// Returns true if the event was signaled within `timeout_ms`.
D3D12M_EXPORT bool d3d12metal_native_wait_event(HANDLE event, unsigned timeout_ms);
D3D12M_EXPORT void d3d12metal_native_destroy_event(HANDLE event);
