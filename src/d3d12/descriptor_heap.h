// ID3D12DescriptorHeap and the CPU-side descriptor layouts.
#pragma once

#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

// Descriptor slot of an RTV heap. Every heap type uses 24-byte slots, the size
// of a shader-visible mtlb_descriptor, so copies between heaps are plain memcpy.
struct RenderTargetDescriptor {
    mtlb_texture texture;
    uint32_t view_format;  // mtlb_format to view the texture as; 0 = its own format
    uint32_t mip_level;
    uint32_t array_slice;
    uint32_t reserved;
};

constexpr UINT kDescriptorSize = sizeof(mtlb_descriptor);
static_assert(sizeof(RenderTargetDescriptor) == kDescriptorSize, "descriptor slots must share one size");

class DescriptorHeap final : public ChildImpl<ID3D12DescriptorHeap> {
public:
    static HRESULT create(Device *device, const D3D12_DESCRIPTOR_HEAP_DESC &desc, REFIID riid, void **out);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(this, riid, out);
    }

    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override;
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override;

private:
    explicit DescriptorHeap(Device *device) : ChildImpl(device) {}
    ~DescriptorHeap() override;

    D3D12_DESCRIPTOR_HEAP_DESC desc_{};
    uint8_t *storage_ = nullptr;           // CPU view of the slots
    std::vector<uint8_t> host_storage_;    // backs storage_ unless the heap is shader visible
    mtlb_buffer buffer_ = 0;               // shader-visible heaps live in a bridge buffer
    uint64_t gpu_address_ = 0;
};

} // namespace d3d12m
