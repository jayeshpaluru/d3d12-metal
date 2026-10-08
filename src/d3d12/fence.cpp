#include "d3d12/fence.h"

#include <algorithm>

#include "common/platform.h"
#include "d3d12/device.h"

namespace d3d12m {

HRESULT Fence::create(Device *device, UINT64 initial_value, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    auto *fence = new Fence(device);
    if (mtlb_event_create(device->handle(), initial_value, &fence->event_) != MTLB_OK) {
        fence->Release();
        return E_FAIL;
    }
    HRESULT hr = fence->QueryInterface(riid, out);
    fence->Release();
    return hr;
}

Fence::~Fence()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (event_)
        mtlb_event_destroy(event_);
}

UINT64 Fence::GetCompletedValue()
{
    return mtlb_event_completed_value(event_);
}

HRESULT Fence::SetEventOnCompletion(UINT64 value, HANDLE event)
{
    if (!event)
        return mtlb_event_wait_cpu(event_, value, UINT64_MAX) == MTLB_OK ? S_OK : E_FAIL;

    if (mtlb_event_completed_value(event_) >= value) {
        platform_set_event(event);
        return S_OK;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.push_back({value, event});
        if (!thread_.joinable())
            thread_ = std::thread([this] { waiter_loop(); });
    }
    wake_.notify_all();
    return S_OK;
}

// Waits on the backend event for the lowest pending value and signals every
// application event whose value has been reached. The wait has a timeout so a
// newly added lower value, or shutdown, is noticed promptly.
void Fence::waiter_loop()
{
    constexpr uint64_t kPollMs = 10;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (waiters_.empty()) {
            wake_.wait(lock, [this] { return stopping_ || !waiters_.empty(); });
            continue;
        }
        const UINT64 lowest = std::min_element(waiters_.begin(), waiters_.end(),
                                               [](const Waiter &a, const Waiter &b) { return a.value < b.value; })->value;
        lock.unlock();
        mtlb_event_wait_cpu(event_, lowest, kPollMs);
        lock.lock();

        const UINT64 completed = mtlb_event_completed_value(event_);
        auto reached = std::stable_partition(waiters_.begin(), waiters_.end(),
                                             [&](const Waiter &w) { return w.value > completed; });
        for (auto it = reached; it != waiters_.end(); ++it)
            platform_set_event(it->event);
        waiters_.erase(reached, waiters_.end());
    }
}

HRESULT Fence::Signal(UINT64 value)
{
    mtlb_event_signal_cpu(event_, value);
    return S_OK;
}

} // namespace d3d12m
