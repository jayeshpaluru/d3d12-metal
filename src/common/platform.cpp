#include "common/platform.h"

#ifdef _WIN32
#include <windows.h>

namespace d3d12m {

void platform_set_event(HANDLE event)
{
    SetEvent(event);
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
