#include "d3d12/resource.h"

#include "d3d12/device.h"
#include "d3d12/formats.h"

namespace d3d12m {

HRESULT Resource::create_committed(Device *device, const D3D12_HEAP_PROPERTIES &heap,
                                   const D3D12_RESOURCE_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    auto *resource = new Resource(device);
    resource->desc_ = desc;
    resource->heap_ = heap;
    HRESULT hr = resource->is_buffer() ? resource->init_buffer() : resource->init_texture();
    if (FAILED(hr)) {
        resource->Release();
        return hr;
    }
    return hand_out(resource, riid, out);
}

// Every buffer uses shared storage, whatever the heap type.
HRESULT Resource::init_buffer()
{
    if (desc_.Width == 0)
        return E_INVALIDARG;
    mtlb_buffer_info info;
    mtlb_result result = mtlb_buffer_create(device()->handle(), desc_.Width, MTLB_STORAGE_SHARED, &buffer_, &info);
    if (result != MTLB_OK) {
        D3D12M_LOG("buffer creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    cpu_ptr_ = static_cast<uint8_t *>(info.cpu_ptr);
    gpu_address_ = info.gpu_address;
    return S_OK;
}

HRESULT Resource::init_texture()
{
    if (heap_.Type != D3D12_HEAP_TYPE_DEFAULT) {
        D3D12M_LOG("textures in upload/readback/custom heaps are not supported");
        return E_NOTIMPL;
    }
    const mtlb_format format = to_mtlb_format(desc_.Format);
    if (format == MTLB_FORMAT_UNKNOWN || desc_.Width == 0 || desc_.Height == 0)
        return E_INVALIDARG;

    desc_.MipLevels = static_cast<UINT16>(resolve_mip_levels(desc_));

    mtlb_texture_desc td{};
    td.dimension = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? MTLB_TEXTURE_1D
                   : desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MTLB_TEXTURE_3D : MTLB_TEXTURE_2D;
    td.format = format;
    td.width = static_cast<uint32_t>(desc_.Width);
    td.height = desc_.Height;
    td.depth_or_array_size = desc_.DepthOrArraySize;
    td.mip_levels = desc_.MipLevels;
    td.sample_count = desc_.SampleDesc.Count;
    td.storage = MTLB_STORAGE_PRIVATE;
    if (!(desc_.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        td.usage |= MTLB_TEXTURE_USAGE_SHADER_READ;
    if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        td.usage |= MTLB_TEXTURE_USAGE_SHADER_WRITE;
    if (desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        td.usage |= MTLB_TEXTURE_USAGE_RENDER_TARGET;

    mtlb_result result = mtlb_texture_create(device()->handle(), &td, &texture_, nullptr);
    if (result != MTLB_OK) {
        D3D12M_LOG("texture creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    return S_OK;
}

Resource::~Resource()
{
    if (buffer_)
        mtlb_buffer_destroy(buffer_);
    if (texture_)
        mtlb_texture_destroy(texture_);
}

HRESULT Resource::Map(UINT subresource, const D3D12_RANGE *, void **data)
{
    if (!is_buffer() || subresource != 0 || heap_.Type == D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;
    // Shared storage is coherent for the CPU, so the ranges need no handling.
    if (data)
        *data = cpu_ptr_;
    return S_OK;
}

void Resource::Unmap(UINT, const D3D12_RANGE *)
{
}

HRESULT Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS *flags)
{
    if (heap)
        *heap = heap_;
    if (flags)
        *flags = D3D12_HEAP_FLAG_NONE;
    return S_OK;
}

} // namespace d3d12m
