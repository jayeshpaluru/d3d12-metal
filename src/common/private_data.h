// Storage behind the Set/GetPrivateData methods of ID3D12Object and IDXGIObject.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "common/com.h"

namespace d3d12m {

class PrivateData {
public:
    PrivateData() = default;
    PrivateData(const PrivateData &) = delete;
    PrivateData &operator=(const PrivateData &) = delete;
    ~PrivateData();

    // A null `data` (or zero size) removes the entry.
    HRESULT set(REFGUID name, UINT size, const void *data);
    // Stores a counted reference; a null `iface` removes the entry.
    HRESULT set_interface(REFGUID name, const IUnknown *iface);
    HRESULT get(REFGUID name, UINT *size, void *data);

private:
    struct Entry {
        GUID name;
        std::vector<uint8_t> blob; // raw bytes, unused for interface entries
        IUnknown *iface = nullptr; // owned reference, or null for blob entries
    };

    // Callers hold mutex_.
    std::vector<Entry>::iterator find(REFGUID name);
    void erase(REFGUID name);

    std::mutex mutex_;
    std::vector<Entry> entries_;
};

// Adds the private data methods shared by ID3D12Object and IDXGIObject (their
// signatures match) to an interface implementation `Base`.
template <typename Base>
class WithPrivateData : public Base {
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

protected:
    PrivateData private_data_;
};

} // namespace d3d12m
