// Base classes shared by the ID3D12Object-derived COM objects.
#pragma once

#include <cstdint>
#include <cstring>

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

// The command list types this layer can create allocators, lists and queues for.
inline bool supported_list_type(D3D12_COMMAND_LIST_TYPE type)
{
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE
           || type == D3D12_COMMAND_LIST_TYPE_COPY;
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
