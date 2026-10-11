// ID3D12DescriptorHeap and the CPU-side descriptor layouts.
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

// Descriptor slot of an RTV or DSV heap. Every heap type uses 24-byte slots, the size
// of a shader-visible mtlb_descriptor, so copies between heaps are plain memcpy.
struct RenderTargetDescriptor {
    uint64_t resource_id;  // Resource::attachment_id() of the texture; 0 = a null view
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
    bool windowed = false;                     // typed buffer SRV or UAV with entries in the heap's window table
};

// D3D12METAL_TYPED_WINDOWS=0 (typed_windows=0 in the conf) turns the typed buffer windows off (mtlb.h,
// MTLB_TYPED_WINDOWS): typed buffer views then keep their first 2^28 elements.
bool typed_windows_enabled();

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

    // The window table of CBV/SRV/UAV heaps (MTLB_TYPED_WINDOWS entries per descriptor, see mtlb.h): the windows of
    // descriptor `index`, or null. Shader-visible heaps have the table from the start (root arguments point into it);
    // the others get one when `create` asks (the first windowed view written into them). Only typed buffer views past
    // 2^28 elements write entries (and views replacing them, a null view).
    mtlb_descriptor *windows(uint32_t index, bool create);
    // GPU address of the window table's first entry; 0 when there is none.
    uint64_t windows_gpu_address() const { return windows_gpu_address_; }
    // Whether a windowed view was ever written into the heap (copies from other heaps skip their window entries).
    bool has_windowed_views() const { return has_windowed_views_; }
    void note_windowed_view() { has_windowed_views_ = true; }

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
    std::atomic<mtlb_descriptor *> windows_{nullptr};
    std::mutex windows_mutex_;                   // creates host_windows_ (views are written from many threads)
    std::vector<mtlb_descriptor> host_windows_;  // backs windows_ unless the heap is shader visible
    mtlb_buffer windows_buffer_ = 0;
    uint64_t windows_gpu_address_ = 0;
    std::atomic<bool> has_windowed_views_{false};
};

} // namespace d3d12m
