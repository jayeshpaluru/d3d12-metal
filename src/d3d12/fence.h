// ID3D12Fence backed by a Metal shared event.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class Fence final : public ChildImpl<ID3D12Fence1> {
public:
    static HRESULT create(Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **out);

    mtlb_event event() const { return event_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Fence, ID3D12Fence1>(this, riid, out);
    }

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;
    D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

private:
    explicit Fence(Device *device) : ChildImpl(device) {}
    ~Fence() override;

    mtlb_event event_ = 0;
    D3D12_FENCE_FLAGS flags_ = D3D12_FENCE_FLAG_NONE;
};

// Signals application events when fences reach the values they wait for. One
// per device: a single ordinary thread of this module (a Windows thread in the
// PE build, never a Metal callback thread) blocks in the bridge until a fence
// notification arrives, then sets the events whose values were reached.
class FenceWaiter {
public:
    FenceWaiter() = default;
    ~FenceWaiter();

    // Sets `event` once `fence` reaches `value`.
    HRESULT add(Fence *fence, UINT64 value, HANDLE event);
    // Calls `callback`, on the waiter thread, once `fence` reaches `value`. It must
    // not wait for anything the waiter thread serves, and it does not run once the
    // fence is destroyed (one already running is not waited for: capture shared state).
    HRESULT add(Fence *fence, UINT64 value, std::function<void()> callback);
    // Drops the pending waits of a fence that is being destroyed.
    void forget(Fence *fence);

private:
    using Wait = std::function<void()>;

    void run();

    std::mutex mutex_;
    mtlb_notify queue_ = 0;
    std::thread thread_;
    // Pending waits per fence, keyed by the value awaited.
    std::unordered_map<Fence *, std::multimap<UINT64, Wait>> waits_;
};

} // namespace d3d12m
