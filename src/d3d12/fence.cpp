#include "d3d12/fence.h"

#include <vector>

#include "common/platform.h"
#include "d3d12/device.h"

namespace d3d12m {

HRESULT Fence::create(Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    auto *fence = new Fence(device);
    fence->flags_ = flags;
    uint64_t *mirror = nullptr;
    mtlb_result result = mtlb_event_create(device->handle(), initial_value, &fence->event_, &mirror);
    if (result == MTLB_OK && !mirror)
        result = MTLB_ERROR_DEVICE;
    fence->mirror_ = reinterpret_cast<std::atomic<UINT64> *>(mirror);
    if (result != MTLB_OK) {
        fence->Release();
        return to_hresult(result);
    }
    return hand_out(fence, riid, out);
}

Fence::~Fence()
{
    device()->fence_waiter().forget(this);
    if (event_)
        mtlb_event_destroy(event_);
}

UINT64 Fence::GetCompletedValue()
{
    D3D12M_TRACE();
    return mirror_->load(std::memory_order_acquire);
}

UINT64 Fence::refresh()
{
    // The mirror takes the event's value as it is (a signal may have lowered it), read again after the store so that a
    // signal landing in between is not overwritten by the older value.
    for (;;) {
        const UINT64 value = mtlb_event_completed_value(event_);
        mirror_->store(value, std::memory_order_release);
        if (mtlb_event_completed_value(event_) == value)
            return value;
    }
}

HRESULT Fence::SetEventOnCompletion(UINT64 value, HANDLE event)
{
    D3D12M_TRACED_BEGIN
    if (!event) {
        const mtlb_result result = mtlb_event_wait_cpu(event_, value, UINT64_MAX);
        if (result == MTLB_OK)
            refresh();  // a caller that waited expects GetCompletedValue() >= value
        return to_hresult(result);
    }
    if (GetCompletedValue() >= value) {
        platform_set_event(event);
        return S_OK;
    }
    return device()->fence_waiter().add(this, value, event);
    D3D12M_TRACED_END(value, event)
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
    return add(fence, value, [event] { platform_set_event(event); });
}

HRESULT FenceWaiter::add(Fence *fence, UINT64 value, std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_) {
        if (mtlb_result result = mtlb_notify_create(&queue_); result != MTLB_OK)
            return to_hresult(result);
        thread_ = std::thread([this] { run(); });
    }
    auto &pending = waits_[fence];
    const auto wait = pending.emplace(value, std::move(callback));
    // Registered under the lock so the entry exists before the notification can
    // arrive; the bridge fires at once if the value was reached meanwhile.
    const mtlb_result result = mtlb_event_notify(fence->event(), value, queue_, reinterpret_cast<uint64_t>(fence));
    if (result != MTLB_OK) {
        pending.erase(wait);  // nothing will ever signal it
        if (pending.empty())
            waits_.erase(fence);
    }
    return to_hresult(result);
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
    std::vector<Wait> reached;
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
                const UINT64 completed = it->first->refresh();
                auto &pending = it->second;
                while (!pending.empty() && pending.begin()->first <= completed) {
                    reached.push_back(std::move(pending.begin()->second));
                    pending.erase(pending.begin());
                }
                if (pending.empty())
                    waits_.erase(it);
            }
        }
        for (Wait &wait : reached)
            wait();
    }
}

HRESULT Fence::Signal(UINT64 value)
{
    D3D12M_TRACED_BEGIN
    mtlb_event_signal_cpu(event_, value);
    return S_OK;
    D3D12M_TRACED_END(value)
}

} // namespace d3d12m
