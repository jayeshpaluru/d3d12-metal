// SPDX-License-Identifier: LGPL-2.1-or-later
// Operating system services the front-end needs but cannot get portably.
//
// The front-end builds natively on macOS (headless tests) and, later, as PE
// DLLs under Wine. The native build has no Win32 events, so it ships a minimal
// event object; the PE build maps the same calls onto the real Win32 API.
#pragma once

#include <cstdint>
#include <string>

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
// A manual-reset event, or null.
HANDLE platform_create_event(bool signaled);
void platform_reset_event(HANDLE event);
// A new handle to the same event, or null.
HANDLE platform_duplicate_event(HANDLE event);
void platform_close_event(HANDLE event);

// The name of the running executable without directory or extension ("game" for C:\\Games\\game.exe).
std::string platform_executable_name();

// The CPU clock the application sees (QueryPerformanceCounter), for GetClockCalibration.
uint64_t platform_performance_counter();

// The text of a marker or event (ID3D12GraphicsCommandList::SetMarker): metadata 1 is an ANSI string, 0 a
// UTF-16 one; PIX's binary encodings get a generic label.
inline std::string marker_text(UINT metadata, const void *data, UINT size)
{
    if (!data || !size)
        return std::string();
    if (metadata == 1) {
        const char *s = static_cast<const char *>(data);
        size_t length = 0;
        while (length < size && s[length])
            ++length;
        return std::string(s, length);
    }
    if (metadata == 0) {
        const uint16_t *w = static_cast<const uint16_t *>(data);
        std::string text;
        for (UINT i = 0; i < size / 2 && w[i]; ++i)
            text.push_back(w[i] < 0x80 ? static_cast<char>(w[i]) : '?');
        return text;
    }
    return "event";
}

} // namespace d3d12m

// Native-build event objects, exported for the tests. They stand in for
// CreateEvent / WaitForSingleObject / CloseHandle. A successful wait consumes
// the signal (auto-reset).
D3D12M_EXPORT HANDLE d3d12metal_native_create_event();
// Returns true if the event was signaled within `timeout_ms`.
D3D12M_EXPORT bool d3d12metal_native_wait_event(HANDLE event, unsigned timeout_ms);
D3D12M_EXPORT void d3d12metal_native_destroy_event(HANDLE event);
