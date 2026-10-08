// ID3D12CommandAllocator. Command lists record into their own streams, so the
// allocator holds no state.
#pragma once

#include "d3d12/object.h"

namespace d3d12m {

class CommandAllocator final : public ChildImpl<ID3D12CommandAllocator> {
public:
    static HRESULT create(Device *device, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandAllocator>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE Reset() override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }

private:
    explicit CommandAllocator(Device *device) : ChildImpl(device) {}
};

inline HRESULT CommandAllocator::create(Device *device, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!supported_list_type(type))
        return E_INVALIDARG;
    auto *allocator = new CommandAllocator(device);
    return hand_out(allocator, riid, out);
}

} // namespace d3d12m
