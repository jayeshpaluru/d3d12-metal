// ID3D12DescriptorHeap and the CPU-side descriptor layouts.
#pragma once

#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

// Descriptor slot of an RTV or DSV heap. Every heap type uses 24-byte slots, the size
// of a shader-visible mtlb_descriptor, so copies between heaps are plain memcpy.
struct RenderTargetDescriptor {
    mtlb_texture texture;
    uint32_t view_format;  // mtlb_format to view the texture as; 0 = its own format
    uint32_t mip_level;
    uint32_t array_slice;  // array slice; the depth plane of a 3D texture
    uint32_t flags;        // DSVs: MTLB_DEPTH_READ_ONLY | MTLB_STENCIL_READ_ONLY
};

constexpr UINT kDescriptorSize = sizeof(mtlb_descriptor);
static_assert(sizeof(RenderTargetDescriptor) == kDescriptorSize, "descriptor slots must share one size");

// The increment between descriptors of a heap type. Every type uses the 24-byte slot of a shader-visible
// descriptor today, so copies between heaps are plain memcpy; the per-type function is the one place to
// change when a type needs more.
inline UINT descriptor_size(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return kDescriptorSize;
    default:
        return 0;
    }
}

// What a UAV descriptor was created from. The descriptor itself holds only the Metal object ids the shader
// converter reads, so operations that need the view (clearing it) look it up here, by the CPU
// handle, in the shadow table of the heap the handle points into.
struct ViewInfo {
    enum Kind : uint8_t { None, Buffer, Texture } kind = None;
    bool raw = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // of the view; unknown for raw and structured buffers
    uint32_t stride = 0;                       // structured buffers
    uint64_t first_element = 0, num_elements = 0;
    mtlb_view_type type = MTLB_VIEW_2D;        // textures
    uint32_t first_mip = 0, first_slice = 0, slice_count = 1;
};

class DescriptorHeap final : public ChildImpl<ID3D12DescriptorHeap> {
public:
    static HRESULT create(Device *device, const D3D12_DESCRIPTOR_HEAP_DESC &desc, REFIID riid, void **out);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(this, riid, out);
    }

    D3D12M_AGGREGATE_RETURN(D3D12_DESCRIPTOR_HEAP_DESC, GetDesc, desc_)
    D3D12M_AGGREGATE_RETURN(D3D12_CPU_DESCRIPTOR_HANDLE, GetCPUDescriptorHandleForHeapStart, cpu_start())
    D3D12M_AGGREGATE_RETURN(D3D12_GPU_DESCRIPTOR_HANDLE, GetGPUDescriptorHandleForHeapStart, gpu_start())

    D3D12_DESCRIPTOR_HEAP_TYPE type() const { return desc_.Type; }
    // GPU address of the first descriptor; 0 for heaps that are not shader visible.
    uint64_t gpu_address() const { return gpu_address_; }
    // The CPU memory of the descriptors, and the shadow info of descriptor `index` (CBV/SRV/UAV heaps only).
    const uint8_t *storage() const { return storage_; }
    uint32_t count() const { return desc_.NumDescriptors; }
    ViewInfo *shadow(uint32_t index) { return index < shadow_.size() ? &shadow_[index] : nullptr; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start() const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start() const;

    explicit DescriptorHeap(Device *device) : ChildImpl(device) {}
    ~DescriptorHeap() override;

    D3D12_DESCRIPTOR_HEAP_DESC desc_{};
    uint8_t *storage_ = nullptr;           // CPU view of the slots
    std::vector<uint8_t> host_storage_;    // backs storage_ unless the heap is shader visible
    mtlb_buffer buffer_ = 0;               // shader-visible heaps live in a bridge buffer
    uint64_t gpu_address_ = 0;
    std::vector<ViewInfo> shadow_;
};

} // namespace d3d12m
