#include "d3d12/object.h"

#include <cstring>

namespace d3d12m {

PrivateDataStore::~PrivateDataStore()
{
    for (auto &[key, entry] : entries_)
        safe_release(entry.iface);
}

void PrivateDataStore::erase_locked(const Key &key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    safe_release(it->second.iface);
    entries_.erase(it);
}

HRESULT PrivateDataStore::get(REFGUID guid, UINT *size, void *data)
{
    if (!size)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(Key{guid});
    if (it == entries_.end()) {
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    const Entry &entry = it->second;
    // Interface entries are reported as the raw pointer, like the native runtime.
    const UINT needed = entry.iface ? sizeof(IUnknown *) : static_cast<UINT>(entry.bytes.size());
    const UINT capacity = *size;
    *size = needed;
    if (!data)
        return S_OK;
    if (capacity < needed)
        return DXGI_ERROR_MORE_DATA;
    if (entry.iface) {
        entry.iface->AddRef();
        std::memcpy(data, &entry.iface, sizeof(IUnknown *));
    } else {
        std::memcpy(data, entry.bytes.data(), needed);
    }
    return S_OK;
}

HRESULT PrivateDataStore::set(REFGUID guid, UINT size, const void *data)
{
    if (size && !data)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(mutex_);
    erase_locked(Key{guid});
    if (size == 0)
        return S_OK;  // setting zero bytes removes the entry
    Entry &entry = entries_[Key{guid}];
    entry.bytes.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    return S_OK;
}

HRESULT PrivateDataStore::set_interface(REFGUID guid, const IUnknown *iface)
{
    std::lock_guard<std::mutex> lock(mutex_);
    erase_locked(Key{guid});
    if (!iface)
        return S_OK;
    Entry &entry = entries_[Key{guid}];
    entry.iface = const_cast<IUnknown *>(iface);
    entry.iface->AddRef();
    return S_OK;
}

} // namespace d3d12m
