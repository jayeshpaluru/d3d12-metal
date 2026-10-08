// Base classes shared by the ID3D12Object-derived COM objects.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "common/com.h"
#include "common/log.h"

namespace d3d12m {

class Device;

// Private data storage behind ID3D12Object::Get/SetPrivateData.
class PrivateDataStore {
public:
    ~PrivateDataStore();
    HRESULT get(REFGUID guid, UINT *size, void *data);
    HRESULT set(REFGUID guid, UINT size, const void *data);
    HRESULT set_interface(REFGUID guid, const IUnknown *iface);

private:
    struct Key {
        GUID guid;
        bool operator<(const Key &o) const { return std::memcmp(&guid, &o.guid, sizeof(GUID)) < 0; }
    };
    struct Entry {
        std::vector<uint8_t> bytes;
        IUnknown *iface = nullptr;  // owned reference when non-null
    };

    void erase_locked(const Key &key);

    std::mutex mutex_;
    std::map<Key, Entry> entries_;
};

// Implements ID3D12Object on top of reference counting. `I` is the most-derived
// interface of the final class.
template <typename I>
class ObjectImpl : public RefCounted<I> {
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *data) override
    {
        return private_data_.get(guid, size, data);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *data) override
    {
        return private_data_.set(guid, size, data);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *iface) override
    {
        return private_data_.set_interface(guid, iface);
    }

    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
    {
        size_t length = 0;
        while (name && name[length])
            ++length;
        return private_data_.set(WKPDID_D3DDebugObjectNameW,
                                 name ? static_cast<UINT>((length + 1) * sizeof(WCHAR)) : 0, name);
    }

private:
    PrivateDataStore private_data_;
};

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
