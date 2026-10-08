#include "d3d12/heap.h"

#include "d3d12/device.h"

namespace d3d12m {

HRESULT Heap::create(Device *device, const D3D12_HEAP_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (desc.SizeInBytes == 0)
        return E_INVALIDARG;
    if (desc.Alignment != 0 && desc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
        && desc.Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
        return E_INVALIDARG;

    auto *heap = new Heap(device);
    heap->desc_ = desc;
    if (heap->desc_.Alignment == 0)
        heap->desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap->storage_ = storage_for_heap(desc.Properties);
    const mtlb_result result = mtlb_heap_create(device->handle(), desc.SizeInBytes, heap->storage_, &heap->heap_);
    if (result != MTLB_OK) {
        D3D12M_LOG("heap creation failed: %s", mtlb_last_error());
        heap->Release();
        return to_hresult(result);
    }
    return hand_out(heap, riid, out);
}

Heap::~Heap()
{
    if (heap_)
        mtlb_heap_destroy(heap_);
}

} // namespace d3d12m
