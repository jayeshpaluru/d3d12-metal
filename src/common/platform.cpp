// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common/platform.h"

#ifndef _WIN32
#include <mach/mach_time.h>
#include <stdlib.h>
#endif

#ifdef _WIN32
#include <windows.h>

namespace d3d12m {

void platform_set_event(HANDLE event)
{
    SetEvent(event);
}

bool platform_window_client_size(HWND window, UINT *width, UINT *height)
{
    RECT rect;
    if (!GetClientRect(window, &rect))
        return false;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    return true;
}

HWND platform_root_window(HWND window)
{
    HWND root = GetAncestor(window, GA_ROOT);
    return root ? root : window;
}

HANDLE platform_create_event(bool signaled)
{
    return CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);
}

std::string platform_executable_name()
{
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string name(path, length < MAX_PATH ? length : 0);
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos)
        name.erase(0, slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0)
        name.erase(dot);
    return name;
}

uint64_t platform_performance_counter()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

void platform_reset_event(HANDLE event)
{
    ResetEvent(event);
}

HANDLE platform_duplicate_event(HANDLE event)
{
    HANDLE copy = nullptr;
    HANDLE process = GetCurrentProcess();
    return DuplicateHandle(process, event, process, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) ? copy : nullptr;
}

void platform_close_event(HANDLE event)
{
    CloseHandle(event);
}

} // namespace d3d12m

#else

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

struct NativeEvent {
    std::mutex mutex;
    std::condition_variable signaled_cv;
    bool signaled = false;
};

} // namespace

namespace d3d12m {

void platform_set_event(HANDLE event)
{
    auto *e = static_cast<NativeEvent *>(event);
    {
        std::lock_guard<std::mutex> lock(e->mutex);
        e->signaled = true;
    }
    e->signaled_cv.notify_all();
}

bool platform_window_client_size(HWND, UINT *, UINT *)
{
    return false;
}

HWND platform_root_window(HWND window)
{
    return window;
}

HANDLE platform_create_event(bool)
{
    return nullptr;
}

std::string platform_executable_name()
{
    return getprogname();
}

uint64_t platform_performance_counter()
{
    return mach_absolute_time();
}

void platform_reset_event(HANDLE)
{
}

HANDLE platform_duplicate_event(HANDLE)
{
    return nullptr;
}

void platform_close_event(HANDLE)
{
}

} // namespace d3d12m

HANDLE d3d12metal_native_create_event()
{
    return new NativeEvent();
}

bool d3d12metal_native_wait_event(HANDLE event, unsigned timeout_ms)
{
    auto *e = static_cast<NativeEvent *>(event);
    std::unique_lock<std::mutex> lock(e->mutex);
    if (!e->signaled_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return e->signaled; }))
        return false;
    e->signaled = false;
    return true;
}

void d3d12metal_native_destroy_event(HANDLE event)
{
    delete static_cast<NativeEvent *>(event);
}

#endif
