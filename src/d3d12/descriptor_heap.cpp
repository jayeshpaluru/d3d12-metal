#include "d3d12/descriptor_heap.h"

#include "d3d12/device.h"

namespace d3d12m {

HRESULT DescriptorHeap::create(Device *device, const D3D12_DESCRIPTOR_HEAP_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (desc.Type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES || desc.NumDescriptors == 0)
        return E_INVALIDARG;
    const bool shader_visible = desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (shader_visible && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
        return E_INVALIDARG;

    auto *heap = new DescriptorHeap(device);
    heap->desc_ = desc;
    if (shader_visible) {
        mtlb_buffer_info info;
        if (mtlb_descriptor_heap_create(device->handle(), desc.NumDescriptors, &heap->buffer_, &info) != MTLB_OK) {
            heap->Release();
            return E_OUTOFMEMORY;
        }
        heap->storage_ = static_cast<uint8_t *>(info.cpu_ptr);
        heap->gpu_address_ = info.gpu_address;
    } else {
        heap->host_storage_.assign(size_t(desc.NumDescriptors) * kDescriptorSize, 0);
        heap->storage_ = heap->host_storage_.data();
    }
    HRESULT hr = heap->QueryInterface(riid, out);
    heap->Release();
    return hr;
}

DescriptorHeap::~DescriptorHeap()
{
    if (buffer_)
        mtlb_buffer_destroy(buffer_);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::GetCPUDescriptorHandleForHeapStart()
{
    return {reinterpret_cast<SIZE_T>(storage_)};
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GetGPUDescriptorHandleForHeapStart()
{
    return {gpu_address_};
}

} // namespace d3d12m
