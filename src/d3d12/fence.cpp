#include "d3d12/fence.h"

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
    if (event_)
        mtlb_event_destroy(event_);
}

UINT64 Fence::GetCompletedValue()
{
    return mtlb_event_completed_value(event_);
}

HRESULT Fence::SetEventOnCompletion(UINT64 value, HANDLE event)
{
    if (event) {
        // TODO: signal Win32 events from a waiter thread once the Wine build exists.
        D3D12M_STUB_HR();
    }
    return mtlb_event_wait_cpu(event_, value, UINT64_MAX) == MTLB_OK ? S_OK : E_FAIL;
}

HRESULT Fence::Signal(UINT64 value)
{
    mtlb_event_signal_cpu(event_, value);
    return S_OK;
}

} // namespace d3d12m
