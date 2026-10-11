// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12/descriptor_heap.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "common/config.h"

#include "d3d12/device.h"

namespace d3d12m {

bool typed_windows_enabled()
{
    static const bool enabled = [] {
        const char *value = config_get("TYPED_WINDOWS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

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
        if (shader_visible && typed_windows_enabled()) {
            mtlb_buffer_info info;
            if (mtlb_descriptor_heap_create(device->handle(), desc.NumDescriptors * MTLB_TYPED_WINDOWS, &heap->windows_buffer_, &info)
                == MTLB_OK) {
                // Null views until windowed views are written: a rewritten shader that indexes an ordinary typed buffer
                // past 2^28 elements reads zero there (and its writes are dropped), as with any out-of-range index.
                auto *windows = static_cast<mtlb_descriptor *>(info.cpu_ptr);
                std::fill(windows, windows + size_t(desc.NumDescriptors) * MTLB_TYPED_WINDOWS,
                          device->null_descriptor(MTLB_NULL_UAV_TYPED_BUFFER));
                heap->windows_.store(windows);
                heap->windows_gpu_address_ = info.gpu_address;
            } else {
                D3D12M_LOG("descriptor heap: no window table (%s)", mtlb_last_error());
            }
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
    if (windows_buffer_)
        mtlb_buffer_destroy(windows_buffer_);
}

mtlb_descriptor *DescriptorHeap::windows(uint32_t index, bool create)
{
    if (index >= shadow_.size())
        return nullptr;
    mtlb_descriptor *table = windows_.load(std::memory_order_acquire);
    if (!table && create && !buffer_) {
        std::lock_guard<std::mutex> lock(windows_mutex_);
        table = windows_.load(std::memory_order_relaxed);
        if (!table) {
            try {
                host_windows_.assign(size_t(desc_.NumDescriptors) * MTLB_TYPED_WINDOWS, mtlb_descriptor{});
            } catch (const std::bad_alloc &) {
                return nullptr;
            }
            table = host_windows_.data();
            windows_.store(table, std::memory_order_release);
        }
    }
    return table ? table + size_t(index) * MTLB_TYPED_WINDOWS : nullptr;
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
