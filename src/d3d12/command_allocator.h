// ID3D12CommandAllocator. Command lists record into their own streams, so the
// allocator only carries the list type.
#pragma once

#include "d3d12/object.h"

namespace d3d12m {

class CommandAllocator final : public ChildImpl<ID3D12CommandAllocator> {
public:
    static HRESULT create(Device *device, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out);

    D3D12_COMMAND_LIST_TYPE type() const { return type_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandAllocator>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }

private:
    CommandAllocator(Device *device, D3D12_COMMAND_LIST_TYPE type) : ChildImpl(device), type_(type) {}

    D3D12_COMMAND_LIST_TYPE type_;
};

inline HRESULT CommandAllocator::create(Device *device, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE
        && type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_INVALIDARG;
    auto *allocator = new CommandAllocator(device, type);
    HRESULT hr = allocator->QueryInterface(riid, out);
    allocator->Release();
    return hr;
}

} // namespace d3d12m
