// Base classes shared by the ID3D12Object-derived COM objects.
#pragma once

#include <cstdint>
#include <cstring>

#include "bridge/mtlb.h"
#include "common/com.h"
#include "common/log.h"
#include "common/private_data.h"

namespace d3d12m {

class Device;

// Implements ID3D12Object on top of reference counting. `I` is the most-derived
// interface of the final class.
template <typename I>
class ObjectImpl : public WithPrivateData<RefCounted<I>> {
public:
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
    {
        size_t length = 0;
        while (name && name[length])
            ++length;
        return this->private_data_.set(WKPDID_D3DDebugObjectNameW,
                                 name ? static_cast<UINT>((length + 1) * sizeof(WCHAR)) : 0, name);
    }
};

// Declares a method of an interface that returns the aggregate `T` (GetDesc and
// friends). The Windows x64 ABI of the MinGW headers passes the result through
// an explicit pointer (`T *Name(T *ret)`) where GCC would pick another
// convention for a by-value return.
#ifdef _WIN32
#define D3D12M_AGGREGATE_RETURN(T, name, expr) \
    T *STDMETHODCALLTYPE name(T *ret) override  \
    {                                           \
        *ret = (expr);                          \
        return ret;                             \
    }
#else
#define D3D12M_AGGREGATE_RETURN(T, name, expr) \
    T STDMETHODCALLTYPE name() override { return (expr); }
#endif

// The castable format list parameter of the newest resource creation methods is
// const in DirectX-Headers and not in the MinGW headers.
#ifdef _WIN32
#define D3D12M_CASTABLE_FORMATS DXGI_FORMAT *
#else
#define D3D12M_CASTABLE_FORMATS const DXGI_FORMAT *
#endif

// The HRESULT for a failed bridge call.
inline HRESULT to_hresult(mtlb_result result)
{
    switch (result) {
    case MTLB_OK: return S_OK;
    case MTLB_ERROR_OUT_OF_MEMORY: return E_OUTOFMEMORY;
    case MTLB_ERROR_INVALID_ARGUMENT: return E_INVALIDARG;
    case MTLB_ERROR_UNSUPPORTED: return E_NOTIMPL;
    default: return E_FAIL;
    }
}

// The command list types this layer can create queues for.
inline bool supported_queue_type(D3D12_COMMAND_LIST_TYPE type)
{
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE
           || type == D3D12_COMMAND_LIST_TYPE_COPY;
}

// The types it can create allocators and lists for: those and bundles, which are recorded like any list and
// inlined into the list that executes them.
inline bool supported_list_type(D3D12_COMMAND_LIST_TYPE type)
{
    return supported_queue_type(type) || type == D3D12_COMMAND_LIST_TYPE_BUNDLE;
}

// Answers ID3D12DeviceChild::GetDevice for `device`.
HRESULT query_device(Device *device, REFIID riid, void **out);

void add_ref_device(Device *device);
void release_device(Device *device);

// Base for objects created by a device: keeps the device alive and implements
// GetDevice. `I` must derive from ID3D12DeviceChild.
template <typename I>
class ChildImpl : public ObjectImpl<I> {
public:
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
    {
        return query_device(device_, riid, device);
    }

protected:
    explicit ChildImpl(Device *device) : device_(device) { add_ref_device(device_); }
    ~ChildImpl() override { release_device(device_); }

    Device *device() const { return device_; }

private:
    Device *device_;
};

} // namespace d3d12m
