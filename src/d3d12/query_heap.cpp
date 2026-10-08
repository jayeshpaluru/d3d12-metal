#include "d3d12/query_heap.h"

#include "d3d12/device.h"

namespace d3d12m {

HRESULT QueryHeap::create(Device *device, const D3D12_QUERY_HEAP_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (desc.Count == 0)
        return E_INVALIDARG;
    uint32_t kind;
    switch (desc.Type) {
    case D3D12_QUERY_HEAP_TYPE_OCCLUSION: kind = MTLB_QUERY_OCCLUSION; break;
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP: kind = MTLB_QUERY_TIMESTAMP; break;
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS: kind = MTLB_QUERY_PIPELINE_STATISTICS; break;
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS: kind = MTLB_QUERY_SO_STATISTICS; break;
    default:
        D3D12M_LOG("query heap type %d is not supported", static_cast<int>(desc.Type));
        return E_NOTIMPL;
    }
    auto *heap = new QueryHeap(device);
    heap->desc_ = desc;
    const mtlb_result result = mtlb_query_heap_create(device->handle(), kind, desc.Count, &heap->heap_);
    if (result != MTLB_OK) {
        D3D12M_LOG("query heap creation failed: %s", mtlb_last_error());
        heap->Release();
        return to_hresult(result);
    }
    return hand_out(heap, riid, out);
}

QueryHeap::~QueryHeap()
{
    if (heap_)
        mtlb_query_heap_destroy(heap_);
}

} // namespace d3d12m
