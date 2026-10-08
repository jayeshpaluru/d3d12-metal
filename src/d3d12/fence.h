// ID3D12Fence backed by a Metal shared event.
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class Fence final : public ChildImpl<ID3D12Fence> {
public:
    static HRESULT create(Device *device, UINT64 initial_value, REFIID riid, void **out);

    mtlb_event event() const { return event_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Fence>(this, riid, out);
    }

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;

private:
    explicit Fence(Device *device) : ChildImpl(device) {}
    ~Fence() override;

    // An application event waiting for the fence to reach `value`.
    struct Waiter {
        UINT64 value;
        HANDLE event;
    };

    void waiter_loop();

    mtlb_event event_ = 0;

    // Events are signaled by one lazily started thread. It is an ordinary
    // thread of this module (a Windows thread in the PE build), never a Metal
    // callback thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Waiter> waiters_;
    std::thread thread_;
    bool stopping_ = false;
};

} // namespace d3d12m
