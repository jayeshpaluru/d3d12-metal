#include "common/private_data.h"

#include <algorithm>
#include <cstring>

namespace d3d12m {

PrivateData::~PrivateData()
{
    for (Entry &e : entries_)
        safe_release(e.iface);
}

std::vector<PrivateData::Entry>::iterator PrivateData::find(REFGUID name)
{
    return std::find_if(entries_.begin(), entries_.end(),
                        [&](const Entry &e) { return e.name == name; });
}

void PrivateData::erase(REFGUID name)
{
    auto it = find(name);
    if (it == entries_.end())
        return;
    safe_release(it->iface);
    entries_.erase(it);
}

HRESULT PrivateData::set(REFGUID name, UINT size, const void *data)
{
    std::lock_guard lock(mutex_);
    erase(name);
    if (!data || size == 0)
        return S_OK;
    Entry e;
    e.name = name;
    e.blob.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    entries_.push_back(std::move(e));
    return S_OK;
}

HRESULT PrivateData::set_interface(REFGUID name, const IUnknown *iface)
{
    std::lock_guard lock(mutex_);
    erase(name);
    if (!iface)
        return S_OK;
    Entry e;
    e.name = name;
    e.iface = const_cast<IUnknown *>(iface);
    e.iface->AddRef();
    entries_.push_back(std::move(e));
    return S_OK;
}

HRESULT PrivateData::get(REFGUID name, UINT *size, void *data)
{
    if (!size)
        return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    auto it = find(name);
    if (it == entries_.end()) {
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    const UINT needed = it->iface ? sizeof(void *) : static_cast<UINT>(it->blob.size());
    if (!data || *size < needed) {
        *size = needed;
        return data ? DXGI_ERROR_MORE_DATA : S_OK;
    }
    *size = needed;
    if (it->iface) {
        it->iface->AddRef();
        std::memcpy(data, &it->iface, sizeof(void *));
    } else {
        std::memcpy(data, it->blob.data(), needed);
    }
    return S_OK;
}

} // namespace d3d12m
