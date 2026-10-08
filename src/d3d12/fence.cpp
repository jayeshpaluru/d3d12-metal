#include "d3d12/fence.h"

#include <vector>

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
    device()->fence_waiter().forget(this);
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
    if (GetCompletedValue() >= value) {
        platform_set_event(event);
        return S_OK;
    }
    return device()->fence_waiter().add(this, value, event);
}

FenceWaiter::~FenceWaiter()
{
    if (!queue_)
        return;
    mtlb_notify_close(queue_);
    thread_.join();
    mtlb_notify_destroy(queue_);
}

HRESULT FenceWaiter::add(Fence *fence, UINT64 value, HANDLE event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_) {
        if (mtlb_notify_create(&queue_) != MTLB_OK)
            return E_FAIL;
        thread_ = std::thread([this] { run(); });
    }
    waits_[fence].emplace(value, event);
    // Registered under the lock so the entry exists before the notification can
    // arrive; the bridge fires at once if the value was reached meanwhile.
    return mtlb_event_notify(fence->event(), value, queue_, reinterpret_cast<uint64_t>(fence)) == MTLB_OK ? S_OK : E_FAIL;
}

void FenceWaiter::forget(Fence *fence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    waits_.erase(fence);
}

void FenceWaiter::run()
{
    mtlb_notification batch[16];
    uint32_t count = 0;
    std::vector<HANDLE> reached;
    while (mtlb_notify_wait(queue_, batch, 16, &count) == MTLB_OK && count) {
        reached.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (uint32_t i = 0; i < count; ++i) {
                // The cookie may name a fence that was destroyed (or whose address
                // was reused); looking it up in waits_ makes that harmless.
                auto it = waits_.find(reinterpret_cast<Fence *>(batch[i].cookie));
                if (it == waits_.end())
                    continue;
                const UINT64 completed = it->first->GetCompletedValue();
                auto &pending = it->second;
                while (!pending.empty() && pending.begin()->first <= completed) {
                    reached.push_back(pending.begin()->second);
                    pending.erase(pending.begin());
                }
                if (pending.empty())
                    waits_.erase(it);
            }
        }
        for (HANDLE event : reached)
            platform_set_event(event);
    }
}

HRESULT Fence::Signal(UINT64 value)
{
    mtlb_event_signal_cpu(event_, value);
    return S_OK;
}

} // namespace d3d12m
