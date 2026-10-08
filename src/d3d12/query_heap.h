// ID3D12QueryHeap.
#pragma once

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class QueryHeap final : public ChildImpl<ID3D12QueryHeap> {
public:
    static HRESULT create(Device *device, const D3D12_QUERY_HEAP_DESC &desc, REFIID riid, void **out);

    mtlb_query_heap handle() const { return heap_; }
    UINT count() const { return desc_.Count; }
    D3D12_QUERY_HEAP_TYPE type() const { return desc_.Type; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12QueryHeap>(this, riid, out);
    }

private:
    explicit QueryHeap(Device *device) : ChildImpl(device) {}
    ~QueryHeap() override;

    D3D12_QUERY_HEAP_DESC desc_{};
    mtlb_query_heap heap_ = 0;
};

// Whether a query of `type` can be made in a heap of `heap_type`, and the bridge's kind for the type.
inline bool query_matches_heap(D3D12_QUERY_TYPE type, D3D12_QUERY_HEAP_TYPE heap_type)
{
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION: return heap_type == D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    case D3D12_QUERY_TYPE_TIMESTAMP: return heap_type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return heap_type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    default: return heap_type == D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
    }
}

} // namespace d3d12m
