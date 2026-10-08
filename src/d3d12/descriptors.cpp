// Shader resource views, unordered access views and samplers: what each CreateXxx call writes into a
// descriptor slot. Slots are mtlb_descriptor entries, the layout the shader converter reads (see
// docs/ARCHITECTURE.md), written straight into the heap's CPU memory; the bridge is asked for the
// Metal object ids (texture views, texture buffer views, samplers).
#include <algorithm>
#include <cstring>

#include "d3d12/descriptor_heap.h"
#include "d3d12/device.h"
#include "d3d12/formats.h"
#include "d3d12/resource.h"
#include "d3d12/sampler.h"

namespace d3d12m {

namespace {

constexpr UINT kAllMips = UINT_MAX;  // "-1" in MipLevels / ArraySize

mtlb_descriptor *slot_of(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    return reinterpret_cast<mtlb_descriptor *>(handle.ptr);
}

// The view's mip and slice ranges, resolved against the resource.
struct Range {
    UINT first_mip = 0, mip_count = 1;
    UINT first_slice = 0, slice_count = 1;
};

struct TextureViewParams {
    mtlb_view_type type;
    DXGI_FORMAT format;
    Range range;
    UINT component_mapping = 0;
    float min_lod = 0;
};

HRESULT make_texture_descriptor(Resource &resource, const TextureViewParams &p, mtlb_descriptor *slot)
{
    mtlb_texture_view_desc view{};
    view.type = p.type;
    const mtlb_format format = to_mtlb_format(p.format);
    // The texture's own format needs no view; DXGI_FORMAT_UNKNOWN means it.
    view.format = format == to_mtlb_format(resource.desc().Format) ? 0 : format;
    view.first_mip = p.range.first_mip;
    view.mip_count = p.range.mip_count;
    view.first_slice = p.range.first_slice;
    view.slice_count = p.range.slice_count;
    view.component_mapping = p.component_mapping;
    uint64_t id = 0;
    HRESULT hr = resource.texture_view(view, &id);
    if (FAILED(hr))
        return hr;
    uint32_t lod_bits;
    std::memcpy(&lod_bits, &p.min_lod, sizeof(lod_bits));
    *slot = {0, id, lod_bits};
    return S_OK;
}

// Resolves a "-1 means all the rest" count against what the resource has after `first`.
UINT resolve_count(UINT count, UINT first, UINT total)
{
    if (first >= total)
        return 0;
    return count == kAllMips ? total - first : std::min(count, total - first);
}

// Typed buffer element size.
bool element_size(DXGI_FORMAT format, UINT *bytes)
{
    mtlb_format_info info;
    if (!get_format_info(format, &info))
        return false;
    *bytes = info.bytes_per_block;
    return true;
}

// Describes the part of a buffer a view covers. Returns false (after logging) for views that
// cannot be built.
struct BufferViewParams {
    UINT64 first_element, num_elements;
    UINT stride;             // structured buffers
    bool raw;
    DXGI_FORMAT format;      // typed buffers
    Resource *counter = nullptr;
    UINT64 counter_offset = 0;
};

HRESULT make_buffer_descriptor(Resource &resource, const BufferViewParams &p, mtlb_descriptor *slot)
{
    mtlb_buffer_view_desc view{};
    view.buffer = resource.buffer();
    UINT bytes = 0;
    if (p.raw) {
        bytes = 4;
    } else if (p.format != DXGI_FORMAT_UNKNOWN) {
        if (!element_size(p.format, &bytes)) {
            D3D12M_LOG("buffer view format %d is not supported", static_cast<int>(p.format));
            return E_INVALIDARG;
        }
        view.format = to_mtlb_format(p.format);
        view.num_elements = static_cast<uint32_t>(std::min<UINT64>(p.num_elements, UINT32_MAX));
    } else if (p.stride) {
        bytes = p.stride;
    } else {
        D3D12M_LOG("buffer view needs a format, a stride or the raw flag");
        return E_INVALIDARG;
    }
    view.offset = p.first_element * bytes;
    view.size = p.num_elements * bytes;
    if (view.offset > resource.desc().Width) {
        D3D12M_LOG("buffer view starts past the end of the buffer");
        return E_INVALIDARG;
    }
    if (p.counter) {
        view.counter_buffer = p.counter->buffer();
        view.counter_offset = p.counter_offset;
    }
    const mtlb_result result = mtlb_buffer_view(&view, slot);
    if (result != MTLB_OK) {
        D3D12M_LOG("buffer view creation failed: %s", mtlb_last_error());
        return to_hresult(result);
    }
    return S_OK;
}

// UAVs and SRVs of a null resource: the bridge's descriptor that reads zero.
mtlb_null_kind null_srv_kind(D3D12_SRV_DIMENSION dimension, bool typed_buffer)
{
    switch (dimension) {
    case D3D12_SRV_DIMENSION_BUFFER: return typed_buffer ? MTLB_NULL_TYPED_BUFFER : MTLB_NULL_BUFFER;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: return MTLB_NULL_TEXTURE_2D_ARRAY;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: return MTLB_NULL_TEXTURE_2D_MS;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: return MTLB_NULL_TEXTURE_2D_MS_ARRAY;
    case D3D12_SRV_DIMENSION_TEXTURE3D: return MTLB_NULL_TEXTURE_3D;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: return MTLB_NULL_TEXTURE_CUBE;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: return MTLB_NULL_TEXTURE_CUBE_ARRAY;
    default: return MTLB_NULL_TEXTURE_2D;
    }
}

mtlb_null_kind null_uav_kind(D3D12_UAV_DIMENSION dimension, bool typed_buffer)
{
    switch (dimension) {
    case D3D12_UAV_DIMENSION_BUFFER: return typed_buffer ? MTLB_NULL_UAV_TYPED_BUFFER : MTLB_NULL_BUFFER;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: return MTLB_NULL_UAV_TEXTURE_2D_ARRAY;
    case D3D12_UAV_DIMENSION_TEXTURE3D: return MTLB_NULL_UAV_TEXTURE_3D;
    default: return MTLB_NULL_UAV_TEXTURE_2D;
    }
}

} // namespace

mtlb_descriptor Device::null_descriptor(uint32_t kind)
{
    std::lock_guard<std::mutex> lock(null_mutex_);
    if (kind >= null_descriptors_.size())
        return {};
    if (!null_ready_[kind]) {
        if (mtlb_null_descriptor(device_, kind, &null_descriptors_[kind]) != MTLB_OK)
            D3D12M_LOG("null descriptor %u unavailable: %s", kind, mtlb_last_error());
        null_ready_[kind] = true;
    }
    return null_descriptors_[kind];
}

void Device::CreateShaderResourceView(ID3D12Resource *resource_ptr, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    mtlb_descriptor *slot = slot_of(dest);
    auto *resource = ours<Resource>(resource_ptr);
    if (!resource) {
        const bool typed = desc && desc->Format != DXGI_FORMAT_UNKNOWN && !(desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW);
        *slot = null_descriptor(desc ? null_srv_kind(desc->ViewDimension, typed) : MTLB_NULL_TEXTURE_2D);
        return;
    }

    const D3D12_RESOURCE_DESC &rd = resource->desc();
    D3D12_SHADER_RESOURCE_VIEW_DESC local = {};
    if (!desc) {
        // The default view: the resource's own format, every mip and slice.
        local.Format = rd.Format;
        local.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const bool array = rd.DepthOrArraySize > 1;
        switch (rd.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER:
            local.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            local.Buffer.NumElements = static_cast<UINT>(rd.Width / 4);
            local.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            local.Format = DXGI_FORMAT_R32_TYPELESS;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            local.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
            local.Texture1DArray = {0, kAllMips, 0, rd.DepthOrArraySize, 0.0f};
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            local.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            local.Texture3D = {0, kAllMips, 0.0f};
            break;
        default:
            if (rd.SampleDesc.Count > 1) {
                local.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
                local.Texture2DMSArray = {0, rd.DepthOrArraySize};
            } else {
                local.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
                local.Texture2DArray = {0, kAllMips, 0, rd.DepthOrArraySize, 0, 0.0f};
            }
        }
        desc = &local;
    }

    const UINT mips = resolve_mip_levels(rd);
    const UINT slices = array_size(rd);
    TextureViewParams p{MTLB_VIEW_2D, desc->Format == DXGI_FORMAT_UNKNOWN ? rd.Format : desc->Format, {},
                        desc->Shader4ComponentMapping & 0xfff};
    HRESULT hr = S_OK;
    switch (desc->ViewDimension) {
    case D3D12_SRV_DIMENSION_BUFFER: {
        if (!resource->is_buffer()) {
            *slot = {};
            return;
        }
        BufferViewParams b{};
        b.first_element = desc->Buffer.FirstElement;
        b.num_elements = desc->Buffer.NumElements;
        b.stride = desc->Buffer.StructureByteStride;
        b.raw = desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW;
        b.format = b.stride ? DXGI_FORMAT_UNKNOWN : desc->Format;
        hr = make_buffer_descriptor(*resource, b, slot);
        if (FAILED(hr))
            *slot = null_descriptor(MTLB_NULL_BUFFER);
        return;
    }
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        p.type = MTLB_VIEW_2D;
        p.range.first_mip = desc->Texture1D.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->Texture1D.MipLevels, p.range.first_mip, mips);
        p.min_lod = desc->Texture1D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        p.type = MTLB_VIEW_2D_ARRAY;
        p.range.first_mip = desc->Texture1DArray.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->Texture1DArray.MipLevels, p.range.first_mip, mips);
        p.range.first_slice = desc->Texture1DArray.FirstArraySlice;
        p.range.slice_count = resolve_count(desc->Texture1DArray.ArraySize, p.range.first_slice, slices);
        p.min_lod = desc->Texture1DArray.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        p.type = MTLB_VIEW_2D;
        p.range.first_mip = desc->Texture2D.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->Texture2D.MipLevels, p.range.first_mip, mips);
        p.min_lod = desc->Texture2D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        p.type = MTLB_VIEW_2D_ARRAY;
        p.range.first_mip = desc->Texture2DArray.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->Texture2DArray.MipLevels, p.range.first_mip, mips);
        p.range.first_slice = desc->Texture2DArray.FirstArraySlice;
        p.range.slice_count = resolve_count(desc->Texture2DArray.ArraySize, p.range.first_slice, slices);
        p.min_lod = desc->Texture2DArray.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        p.type = MTLB_VIEW_2D_MS;
        p.range.mip_count = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        p.type = MTLB_VIEW_2D_MS_ARRAY;
        p.range.mip_count = 1;
        p.range.first_slice = desc->Texture2DMSArray.FirstArraySlice;
        p.range.slice_count = resolve_count(desc->Texture2DMSArray.ArraySize, p.range.first_slice, slices);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        p.type = MTLB_VIEW_3D;
        p.range.first_mip = desc->Texture3D.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->Texture3D.MipLevels, p.range.first_mip, mips);
        p.min_lod = desc->Texture3D.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        p.type = MTLB_VIEW_CUBE;
        p.range.first_mip = desc->TextureCube.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->TextureCube.MipLevels, p.range.first_mip, mips);
        p.range.slice_count = 6;
        p.min_lod = desc->TextureCube.ResourceMinLODClamp;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        p.type = MTLB_VIEW_CUBE_ARRAY;
        p.range.first_mip = desc->TextureCubeArray.MostDetailedMip;
        p.range.mip_count = resolve_count(desc->TextureCubeArray.MipLevels, p.range.first_mip, mips);
        p.range.first_slice = desc->TextureCubeArray.First2DArrayFace;
        p.range.slice_count = desc->TextureCubeArray.NumCubes * 6;
        p.min_lod = desc->TextureCubeArray.ResourceMinLODClamp;
        break;
    default:
        D3D12M_LOG("CreateShaderResourceView: view dimension %d is not supported", static_cast<int>(desc->ViewDimension));
        *slot = {};
        return;
    }
    if (resource->is_buffer() || p.range.mip_count == 0 || p.range.slice_count == 0) {
        *slot = null_descriptor(null_srv_kind(desc->ViewDimension, false));
        return;
    }
    hr = make_texture_descriptor(*resource, p, slot);
    if (FAILED(hr))
        *slot = null_descriptor(null_srv_kind(desc->ViewDimension, false));
}

void Device::CreateUnorderedAccessView(ID3D12Resource *resource_ptr, ID3D12Resource *counter_ptr,
                                       const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    mtlb_descriptor *slot = slot_of(dest);
    // The shadow view info is stored on every way out (a failed or null view leaves "none").
    ViewInfo info;
    struct Store {
        Device *device;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        ViewInfo &info;
        ~Store()
        {
            if (ViewInfo *shadow = device->view_info(handle))
                *shadow = info;
        }
    } store{this, dest, info};
    auto *resource = ours<Resource>(resource_ptr);
    if (!resource || !desc) {
        const bool typed = desc && desc->Format != DXGI_FORMAT_UNKNOWN && !(desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW);
        *slot = null_descriptor(desc ? null_uav_kind(desc->ViewDimension, typed) : MTLB_NULL_UAV_TEXTURE_2D);
        return;
    }

    const D3D12_RESOURCE_DESC &rd = resource->desc();
    const UINT slices = array_size(rd);
    TextureViewParams p{MTLB_VIEW_2D, desc->Format == DXGI_FORMAT_UNKNOWN ? rd.Format : desc->Format, {}, 0};
    switch (desc->ViewDimension) {
    case D3D12_UAV_DIMENSION_BUFFER: {
        if (!resource->is_buffer()) {
            *slot = {};
            return;
        }
        BufferViewParams b{};
        b.first_element = desc->Buffer.FirstElement;
        b.num_elements = desc->Buffer.NumElements;
        b.stride = desc->Buffer.StructureByteStride;
        b.raw = desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW;
        b.format = b.stride ? DXGI_FORMAT_UNKNOWN : desc->Format;
        b.counter = ours<Resource>(counter_ptr);
        b.counter_offset = desc->Buffer.CounterOffsetInBytes;
        if (FAILED(make_buffer_descriptor(*resource, b, slot))) {
            *slot = null_descriptor(MTLB_NULL_BUFFER);
            return;
        }
        info.kind = ViewInfo::Buffer;
        info.raw = b.raw;
        info.format = b.format;
        info.stride = b.stride;
        info.first_element = b.first_element;
        info.num_elements = b.num_elements;
        return;
    }
    case D3D12_UAV_DIMENSION_TEXTURE1D:
    case D3D12_UAV_DIMENSION_TEXTURE2D:
        p.type = MTLB_VIEW_2D;
        p.range.first_mip = desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1D ? desc->Texture1D.MipSlice
                                                                                  : desc->Texture2D.MipSlice;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
        p.type = MTLB_VIEW_2D_ARRAY;
        p.range.first_mip = desc->Texture1DArray.MipSlice;
        p.range.first_slice = desc->Texture1DArray.FirstArraySlice;
        p.range.slice_count = resolve_count(desc->Texture1DArray.ArraySize, p.range.first_slice, slices);
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
        p.type = MTLB_VIEW_2D_ARRAY;
        p.range.first_mip = desc->Texture2DArray.MipSlice;
        p.range.first_slice = desc->Texture2DArray.FirstArraySlice;
        p.range.slice_count = resolve_count(desc->Texture2DArray.ArraySize, p.range.first_slice, slices);
        break;
    case D3D12_UAV_DIMENSION_TEXTURE3D: {
        p.type = MTLB_VIEW_3D;
        p.range.first_mip = desc->Texture3D.MipSlice;
        const UINT depth = mip_extent(rd.DepthOrArraySize, desc->Texture3D.MipSlice);
        if (desc->Texture3D.FirstWSlice != 0 || (desc->Texture3D.WSize != kAllMips && desc->Texture3D.WSize != depth))
            D3D12M_LOG("a 3D texture UAV over a part of the depth range covers the whole depth");
        break;
    }
    default:
        D3D12M_LOG("CreateUnorderedAccessView: view dimension %d is not supported", static_cast<int>(desc->ViewDimension));
        *slot = {};
        return;
    }
    if (resource->is_buffer() || p.range.slice_count == 0) {
        *slot = null_descriptor(null_uav_kind(desc->ViewDimension, false));
        return;
    }
    if (FAILED(make_texture_descriptor(*resource, p, slot))) {
        *slot = null_descriptor(null_uav_kind(desc->ViewDimension, false));
        return;
    }
    info.kind = ViewInfo::Texture;
    info.format = p.format;
    info.type = p.type;
    info.first_mip = p.range.first_mip;
    info.first_slice = p.range.first_slice;
    info.slice_count = p.type == MTLB_VIEW_3D ? mip_extent(rd.DepthOrArraySize, p.range.first_mip) : p.range.slice_count;
}

void Device::CreateSampler(const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    mtlb_descriptor *slot = slot_of(dest);
    if (!desc) {
        *slot = {};
        return;
    }
    const mtlb_sampler_desc sampler = to_sampler_desc(*desc);
    if (mtlb_sampler_create(device_, &sampler, slot) != MTLB_OK) {
        D3D12M_LOG("sampler creation failed: %s", mtlb_last_error());
        *slot = {};
    }
}

} // namespace d3d12m
