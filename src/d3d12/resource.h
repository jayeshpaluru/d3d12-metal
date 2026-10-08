// ID3D12Resource: committed buffers and textures.
#pragma once

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class Resource final : public ChildImpl<ID3D12Resource> {
public:
    static HRESULT create_committed(Device *device, const D3D12_HEAP_PROPERTIES &heap,
                                    const D3D12_RESOURCE_DESC &desc, REFIID riid, void **out);

    bool is_buffer() const { return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    mtlb_buffer buffer() const { return buffer_; }
    mtlb_texture texture() const { return texture_; }
    const D3D12_RESOURCE_DESC &desc() const { return desc_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12Resource>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE *read_range, void **data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE *written_range) override;
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return gpu_address_; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT, UINT) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT, const D3D12_BOX *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS *flags) override;

private:
    explicit Resource(Device *device) : ChildImpl(device) {}
    ~Resource() override;

    HRESULT init_buffer();
    HRESULT init_texture();

    D3D12_RESOURCE_DESC desc_{};
    D3D12_HEAP_PROPERTIES heap_{};
    mtlb_buffer buffer_ = 0;
    mtlb_texture texture_ = 0;
    uint8_t *cpu_ptr_ = nullptr;
    uint64_t gpu_address_ = 0;
};

} // namespace d3d12m
