// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12/descriptor_heap.h"

#include <new>

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
        mtlb_result result = mtlb_descriptor_heap_create(device->handle(), desc.NumDescriptors, &heap->buffer_, &info);
        if (result != MTLB_OK) {
            heap->Release();
            return to_hresult(result);
        }
        heap->storage_ = static_cast<uint8_t *>(info.cpu_ptr);
        heap->gpu_address_ = info.gpu_address;
    } else {
        try {
            heap->host_storage_.assign(size_t(desc.NumDescriptors) * kDescriptorSize, 0);
        } catch (const std::bad_alloc &) {
            heap->Release();
            return E_OUTOFMEMORY;
        }
        heap->storage_ = heap->host_storage_.data();
    }
    if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        try {
            heap->shadow_.resize(desc.NumDescriptors);
        } catch (const std::bad_alloc &) {
            heap->Release();
            return E_OUTOFMEMORY;
        }
    }
    device->register_heap(heap);
    return hand_out(heap, riid, out);
}

DescriptorHeap::~DescriptorHeap()
{
    device()->unregister_heap(this);
    if (buffer_)
        mtlb_buffer_destroy(buffer_);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::cpu_start() const
{
    return {reinterpret_cast<SIZE_T>(storage_)};
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::gpu_start() const
{
    return {gpu_address_};
}

} // namespace d3d12m
