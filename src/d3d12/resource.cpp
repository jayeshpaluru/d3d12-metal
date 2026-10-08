#include "d3d12/resource.h"

#include "d3d12/device.h"
#include "d3d12/formats.h"

namespace d3d12m {

bool to_texture_desc(const D3D12_RESOURCE_DESC &desc, mtlb_storage storage, mtlb_texture_desc *out)
{
    const mtlb_format format = to_mtlb_format(desc.Format);
    if (format == MTLB_FORMAT_UNKNOWN || desc.Width == 0 || desc.Height == 0)
        return false;
    mtlb_texture_desc td{};
    td.dimension = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? MTLB_TEXTURE_1D
                   : desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MTLB_TEXTURE_3D : MTLB_TEXTURE_2D;
    td.format = format;
    td.width = static_cast<uint32_t>(desc.Width);
    td.height = desc.Height;
    td.depth_or_array_size = desc.DepthOrArraySize;
    td.mip_levels = resolve_mip_levels(desc);
    td.sample_count = desc.SampleDesc.Count;
    td.storage = storage;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        td.usage |= MTLB_TEXTURE_USAGE_SHADER_READ;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        td.usage |= MTLB_TEXTURE_USAGE_SHADER_WRITE;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        td.usage |= MTLB_TEXTURE_USAGE_RENDER_TARGET;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        td.usage |= MTLB_TEXTURE_USAGE_DEPTH_STENCIL;
    *out = td;
    return true;
}

HRESULT Resource::create_committed(Device *device, const D3D12_HEAP_PROPERTIES &heap,
                                   const D3D12_RESOURCE_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    auto *resource = new Resource(device);
    resource->desc_ = desc;
    resource->heap_ = heap;
    HRESULT hr = resource->is_buffer() ? resource->init_buffer(nullptr, 0) : resource->init_texture(nullptr, 0);
    if (FAILED(hr)) {
        resource->Release();
        return hr;
    }
    resource->register_attachment();
    return hand_out(resource, riid, out);
}

HRESULT Resource::create_placed(Device *device, Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC &desc,
                                REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    auto *resource = new Resource(device);
    resource->desc_ = desc;
    resource->heap_ = heap->desc().Properties;
    resource->placed_in_ = heap;
    heap->AddRef();
    HRESULT hr = resource->is_buffer() ? resource->init_buffer(heap, offset) : resource->init_texture(heap, offset);
    if (FAILED(hr)) {
        resource->Release();
        return hr;
    }
    resource->register_attachment();
    // Memory that other resources used keeps their contents. A render target or depth-stencil placed over it
    // is cleared before its first use, so nothing aliased is ever read as colour or depth (vkd3d-proton's
    // "forced initial transition"; see Device::take_pending_init).
    if (!resource->is_buffer() && (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        resource->needs_init_ = true;
        device->add_pending_init(resource);
    }
    return hand_out(resource, riid, out);
}

// Committed buffers use shared storage, whatever the heap type. Placed ones follow their heap.
HRESULT Resource::init_buffer(Heap *heap, UINT64 offset)
{
    if (desc_.Width == 0)
        return E_INVALIDARG;
    mtlb_buffer_info info;
    mtlb_result result;
    if (heap) {
        if (heap->handle() == 0 || offset > heap->desc().SizeInBytes || desc_.Width > heap->desc().SizeInBytes - offset)
            return E_INVALIDARG;
        result = mtlb_buffer_create_in_heap(heap->handle(), offset, desc_.Width, &buffer_, &info);
    } else {
        result = mtlb_buffer_create(device()->handle(), desc_.Width, MTLB_STORAGE_SHARED, &buffer_, &info);
    }
    if (result != MTLB_OK) {
        D3D12M_LOG("buffer creation failed: %s", mtlb_last_error());
        return result == MTLB_ERROR_OUT_OF_MEMORY && heap ? E_INVALIDARG : to_hresult(result);
    }
    cpu_ptr_ = static_cast<uint8_t *>(info.cpu_ptr);
    gpu_address_ = info.gpu_address;
    return S_OK;
}

HRESULT Resource::init_texture(Heap *heap, UINT64 offset)
{
    if (heap_.Type != D3D12_HEAP_TYPE_DEFAULT && !heap) {
        D3D12M_LOG("textures in upload/readback/custom heaps are not supported");
        return E_NOTIMPL;
    }
    if (heap && heap->cpu_visible()) {
        D3D12M_LOG("textures in CPU-visible heaps are not supported");
        return E_NOTIMPL;
    }
    desc_.MipLevels = static_cast<UINT16>(resolve_mip_levels(desc_));
    mtlb_texture_desc td;
    if (!to_texture_desc(desc_, MTLB_STORAGE_PRIVATE, &td))
        return E_INVALIDARG;

    mtlb_result result;
    if (heap) {
        mtlb_size_align size_align;
        if (offset >= heap->desc().SizeInBytes)
            return E_INVALIDARG;
        result = mtlb_texture_size_align(device()->handle(), &td, &size_align);
        if (result == MTLB_OK && (offset % size_align.align != 0 || size_align.size > heap->desc().SizeInBytes - offset)) {
            D3D12M_LOG("placed texture at offset %llu: needs alignment %llu and %llu bytes, the heap has %llu",
                       static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size_align.align),
                       static_cast<unsigned long long>(size_align.size),
                       static_cast<unsigned long long>(heap->desc().SizeInBytes));
            return E_INVALIDARG;
        }
        if (result == MTLB_OK)
            result = mtlb_texture_create_in_heap(heap->handle(), offset, &td, &texture_, nullptr);
    } else {
        result = mtlb_texture_create(device()->handle(), &td, &texture_, nullptr);
    }
    if (result != MTLB_OK) {
        D3D12M_LOG("texture creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    return S_OK;
}

HRESULT Resource::texture_view(const mtlb_texture_view_desc &desc, uint64_t *resource_id)
{
    std::array<uint32_t, 8> key;
    std::memcpy(key.data(), &desc, sizeof(key));
    std::lock_guard<std::mutex> lock(views_mutex_);
    auto it = views_.find(key);
    if (it != views_.end()) {
        *resource_id = it->second;
        return S_OK;
    }
    const mtlb_result result = mtlb_texture_view(texture_, &desc, resource_id);
    if (result != MTLB_OK) {
        D3D12M_LOG("texture view creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    views_.emplace(key, *resource_id);
    return S_OK;
}

HRESULT Resource::buffer_view(const mtlb_buffer_view_desc &desc, mtlb_descriptor *out)
{
    const bool cacheable = desc.counter_buffer == 0;
    const std::array<uint64_t, 4> key{desc.offset, desc.size, desc.format, desc.num_elements};
    if (cacheable) {
        std::lock_guard<std::mutex> lock(views_mutex_);
        auto it = buffer_views_.find(key);
        if (it != buffer_views_.end()) {
            *out = it->second;
            return S_OK;
        }
    }
    const mtlb_result result = mtlb_buffer_view(&desc, out);
    if (result != MTLB_OK) {
        D3D12M_LOG("buffer view creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    if (cacheable) {
        std::lock_guard<std::mutex> lock(views_mutex_);
        buffer_views_.emplace(key, *out);
    }
    return S_OK;
}

void Resource::register_attachment()
{
    if (!is_buffer() && (desc_.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        attachment_id_ = device()->register_attachment(this);
}

Resource::~Resource()
{
    if (attachment_id_)
        device()->unregister_attachment(attachment_id_);  // waits for lookups that hold this resource
    if (needs_init_)
        device()->remove_pending_init(this);
    if (buffer_)
        mtlb_buffer_destroy(buffer_);
    if (texture_)
        mtlb_texture_destroy(texture_);
    safe_release(placed_in_);
}

HRESULT Resource::Map(UINT subresource, const D3D12_RANGE *, void **data)
{
    D3D12M_TRACED_BEGIN
    if (!is_buffer() || subresource != 0 || heap_.Type == D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;
    // Shared storage is coherent for the CPU, so the ranges need no handling.
    if (data)
        *data = cpu_ptr_;
    return S_OK;
    D3D12M_TRACED_END(subresource, data)
}

void Resource::Unmap(UINT, const D3D12_RANGE *)
{
}

HRESULT Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS *flags)
{
    D3D12M_TRACED_BEGIN
    if (heap)
        *heap = heap_;
    if (flags)
        *flags = D3D12_HEAP_FLAG_NONE;
    return S_OK;
    D3D12M_TRACED_END(heap, flags)
}

} // namespace d3d12m
