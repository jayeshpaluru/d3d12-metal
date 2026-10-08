// ID3D12Resource: committed buffers and textures.
#pragma once

#include <array>
#include <map>
#include <mutex>

#include "bridge/mtlb.h"
#include "d3d12/heap.h"
#include "d3d12/object.h"

namespace d3d12m {

// The bridge's description of a texture for a resource (storage as given).
bool to_texture_desc(const D3D12_RESOURCE_DESC &desc, mtlb_storage storage, mtlb_texture_desc *out);

class Resource final : public ChildImpl<ID3D12Resource2> {
public:
    static HRESULT create_committed(Device *device, const D3D12_HEAP_PROPERTIES &heap,
                                    const D3D12_RESOURCE_DESC &desc, REFIID riid, void **out);
    // A resource at `offset` of a heap; it may overlap other resources of the heap.
    static HRESULT create_placed(Device *device, Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC &desc,
                                 REFIID riid, void **out);

    bool is_buffer() const { return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    mtlb_buffer buffer() const { return buffer_; }
    mtlb_texture texture() const { return texture_; }
    const D3D12_RESOURCE_DESC &desc() const { return desc_; }

    // The resource id of a view of this texture for descriptors (see mtlb_texture_view). Applications
    // recreate the same descriptors every frame, so the answers are cached here and the bridge is
    // called once per distinct view.
    HRESULT texture_view(const mtlb_texture_view_desc &desc, uint64_t *resource_id);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource, ID3D12Resource1,
                                ID3D12Resource2>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE *read_range, void **data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE *written_range) override;
    D3D12M_AGGREGATE_RETURN(D3D12_RESOURCE_DESC, GetDesc, desc_)
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return gpu_address_; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT, UINT) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT, const D3D12_BOX *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS *flags) override;
    // ID3D12Resource1
    HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(REFIID, void **) override { return DXGI_ERROR_NOT_FOUND; }
    // ID3D12Resource2
    D3D12M_AGGREGATE_RETURN(D3D12_RESOURCE_DESC1, GetDesc1, desc1())

private:
    explicit Resource(Device *device) : ChildImpl(device) {}
    ~Resource() override;

    D3D12_RESOURCE_DESC1 desc1() const
    {
        return {desc_.Dimension, desc_.Alignment, desc_.Width, desc_.Height, desc_.DepthOrArraySize, desc_.MipLevels,
                desc_.Format, desc_.SampleDesc, desc_.Layout, desc_.Flags, {}};
    }

    HRESULT init_buffer(Heap *heap, UINT64 offset);
    HRESULT init_texture(Heap *heap, UINT64 offset);

    D3D12_RESOURCE_DESC desc_{};
    D3D12_HEAP_PROPERTIES heap_{};
    mtlb_buffer buffer_ = 0;
    mtlb_texture texture_ = 0;
    uint8_t *cpu_ptr_ = nullptr;
    uint64_t gpu_address_ = 0;
    Heap *placed_in_ = nullptr;  // owned reference
    bool needs_init_ = false;

    std::mutex views_mutex_;
    std::map<std::array<uint32_t, 8>, uint64_t> views_;
};

} // namespace d3d12m
