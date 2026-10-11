// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12Heap: a placement heap (MTLHeap) that resources are created in.
#pragma once

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class Heap final : public ChildImpl<ID3D12Heap1> {
public:
    static HRESULT create(Device *device, const D3D12_HEAP_DESC &desc, REFIID riid, void **out);

    mtlb_heap handle() const { return heap_; }
    const D3D12_HEAP_DESC &desc() const { return desc_; }
    // True for heaps the CPU can map (upload, readback, and custom heaps with CPU access).
    bool cpu_visible() const { return storage_ == MTLB_STORAGE_SHARED; }
    mtlb_storage storage() const { return storage_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Heap, ID3D12Heap1>(this, riid, out);
    }

    D3D12M_AGGREGATE_RETURN(D3D12_HEAP_DESC, GetDesc, desc_)
    HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(REFIID, void **) override { D3D12M_TRACED_BEGIN return DXGI_ERROR_NOT_FOUND; D3D12M_TRACED_END() }

private:
    explicit Heap(Device *device) : ChildImpl(device) {}
    ~Heap() override;

    D3D12_HEAP_DESC desc_{};
    mtlb_heap heap_ = 0;
    mtlb_storage storage_ = MTLB_STORAGE_PRIVATE;
};

// The storage a heap of these properties uses: CPU-visible heaps are shared memory.
inline mtlb_storage storage_for_heap(const D3D12_HEAP_PROPERTIES &properties)
{
    switch (properties.Type) {
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        return MTLB_STORAGE_SHARED;
    case D3D12_HEAP_TYPE_CUSTOM:
        return properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE ? MTLB_STORAGE_PRIVATE
                                                                                   : MTLB_STORAGE_SHARED;
    default:
        return MTLB_STORAGE_PRIVATE;
    }
}

} // namespace d3d12m
