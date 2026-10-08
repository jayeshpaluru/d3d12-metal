#include "d3d12/device.h"

#include <algorithm>

#include "d3d12/command_allocator.h"
#include "d3d12/command_list.h"
#include "d3d12/command_queue.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/fence.h"
#include "d3d12/formats.h"
#include "d3d12/pipeline_state.h"
#include "d3d12/resource.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

namespace {

constexpr D3D_FEATURE_LEVEL kMaxFeatureLevel = D3D_FEATURE_LEVEL_12_0;
constexpr D3D_SHADER_MODEL kMaxShaderModel = D3D_SHADER_MODEL_6_6;
constexpr UINT64 kResourceAlignment = 64 * 1024;

// Typed access to the in/out structure of CheckFeatureSupport.
template <typename T>
T *feature_data(void *data, UINT size)
{
    return size == sizeof(T) ? static_cast<T *>(data) : nullptr;
}

D3D12_FORMAT_SUPPORT1 format_support1(const mtlb_format_info &info)
{
    UINT support = 0;
    if (info.flags & MTLB_FORMAT_FLAG_TEXTURE) {
        support |= D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURE3D
                   | D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_SHADER_LOAD
                   | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_MIP;
    }
    if (info.flags & MTLB_FORMAT_FLAG_RENDER_TARGET) {
        support |= (info.flags & MTLB_FORMAT_FLAG_DEPTH) ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
                                                         : D3D12_FORMAT_SUPPORT1_RENDER_TARGET
                                                               | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
    }
    if (info.flags & MTLB_FORMAT_FLAG_BLENDABLE)
        support |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
    if (info.flags & MTLB_FORMAT_FLAG_VERTEX)
        support |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
    return static_cast<D3D12_FORMAT_SUPPORT1>(support);
}

D3D12_FORMAT_SUPPORT2 format_support2(const mtlb_format_info &info)
{
    return (info.flags & MTLB_FORMAT_FLAG_SHADER_WRITE)
               ? D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE
               : D3D12_FORMAT_SUPPORT2_NONE;
}

} // namespace

// ---- Object plumbing -------------------------------------------------------

HRESULT query_device(Device *device, REFIID riid, void **out)
{
    return device->QueryInterface(riid, out);
}

void add_ref_device(Device *device)
{
    device->AddRef();
}

void release_device(Device *device)
{
    device->Release();
}

HRESULT Device::create(ID3D12Device2 **out)
{
    auto *device = new Device();
    if (mtlb_device_create(&device->device_) != MTLB_OK || mtlb_device_get_caps(device->device_, &device->caps_) != MTLB_OK) {
        D3D12M_LOG("no usable Metal device: %s", mtlb_last_error());
        device->Release();
        return DXGI_ERROR_UNSUPPORTED;
    }
    *out = device;
    return S_OK;
}

Device::~Device()
{
    if (device_)
        mtlb_device_destroy(device_);
}

// ---- Creation --------------------------------------------------------------

UINT Device::GetNodeCount()
{
    return 1;
}

HRESULT Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out)
{
    return desc ? CommandQueue::create(this, *desc, riid, out) : E_INVALIDARG;
}

HRESULT Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out)
{
    return CommandAllocator::create(this, type, riid, out);
}

HRESULT Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    return desc ? PipelineState::create_graphics(this, *desc, riid, out) : E_INVALIDARG;
}

HRESULT Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state, REFIID riid, void **out)
{
    return CommandList::create(this, type, allocator, initial_state, riid, out);
}

HRESULT Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out)
{
    return desc ? DescriptorHeap::create(this, *desc, riid, out) : E_INVALIDARG;
}

UINT Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE)
{
    return kDescriptorSize;
}

HRESULT Device::CreateRootSignature(UINT, const void *blob, SIZE_T size, REFIID riid, void **out)
{
    return RootSignature::create(this, blob, size, riid, out);
}

HRESULT Device::CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS, REFIID riid, void **out)
{
    return Fence::create(this, initial_value, riid, out);
}

HRESULT Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *desc,
                                        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out)
{
    return heap && desc ? Resource::create_committed(this, *heap, *desc, riid, out) : E_INVALIDARG;
}

// ---- Descriptors -----------------------------------------------------------

void Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    auto *entry = reinterpret_cast<mtlb_descriptor *>(dest.ptr);
    if (desc)
        mtlb_descriptor_set_buffer(entry, desc->BufferLocation, desc->SizeInBytes);
    else
        *entry = {};
}

void Device::CreateRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    auto *slot = reinterpret_cast<RenderTargetDescriptor *>(dest.ptr);
    *slot = {};
    auto *texture = static_cast<Resource *>(resource);
    if (!texture || texture->is_buffer())
        return;

    slot->texture = texture->texture();
    if (!desc || desc->Format == DXGI_FORMAT_UNKNOWN)
        return;
    // A view of a different format than the texture's own needs a texture view.
    if (to_mtlb_format(desc->Format) != to_mtlb_format(texture->desc().Format))
        slot->view_format = to_mtlb_format(desc->Format);
    switch (desc->ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE2D:
        slot->mip_level = desc->Texture2D.MipSlice;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
        slot->mip_level = desc->Texture2DArray.MipSlice;
        slot->array_slice = desc->Texture2DArray.FirstArraySlice;
        break;
    default:
        break;
    }
}

// All descriptor types use same-sized slots, so copying is a memcpy per range.
void Device::CopyDescriptors(UINT num_dest_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_starts,
                             const UINT *dest_sizes, UINT num_src_ranges,
                             const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes,
                             D3D12_DESCRIPTOR_HEAP_TYPE)
{
    UINT d = 0, s = 0, d_used = 0, s_used = 0;
    while (d < num_dest_ranges && s < num_src_ranges) {
        const UINT dest_size = dest_sizes ? dest_sizes[d] : 1;
        const UINT src_size = src_sizes ? src_sizes[s] : 1;
        const UINT n = std::min(dest_size - d_used, src_size - s_used);
        std::memcpy(reinterpret_cast<void *>(dest_starts[d].ptr + size_t(d_used) * kDescriptorSize),
                    reinterpret_cast<const void *>(src_starts[s].ptr + size_t(s_used) * kDescriptorSize),
                    size_t(n) * kDescriptorSize);
        d_used += n;
        s_used += n;
        if (d_used == dest_size) { ++d; d_used = 0; }
        if (s_used == src_size) { ++s; s_used = 0; }
    }
}

void Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                   D3D12_DESCRIPTOR_HEAP_TYPE)
{
    std::memcpy(reinterpret_cast<void *>(dest.ptr), reinterpret_cast<const void *>(src.ptr),
                size_t(count) * kDescriptorSize);
}

// ---- Resources -------------------------------------------------------------

D3D12_RESOURCE_ALLOCATION_INFO Device::GetResourceAllocationInfo(UINT, UINT count, const D3D12_RESOURCE_DESC *descs)
{
    D3D12_RESOURCE_ALLOCATION_INFO info{0, kResourceAlignment};
    for (UINT i = 0; i < count; ++i) {
        UINT64 size = 0;
        if (!compute_copyable_footprints(descs[i], 0, descs[i].Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                                          ? 1 : resolve_mip_levels(descs[i]) * array_size(descs[i]),
                                         0, nullptr, nullptr, nullptr, &size))
            return {UINT64_MAX, kResourceAlignment};
        info.SizeInBytes += align_up(size, kResourceAlignment);
    }
    return info;
}

void Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base_offset,
                                   D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows,
                                   UINT64 *row_sizes, UINT64 *total_bytes)
{
    if (!compute_copyable_footprints(*desc, first, count, base_offset, layouts, num_rows, row_sizes, total_bytes))
        D3D12M_LOG("GetCopyableFootprints: unsupported format or subresource range");
}

HRESULT Device::MakeResident(UINT, ID3D12Pageable *const *)
{
    return S_OK;  // every allocation is always resident
}

HRESULT Device::Evict(UINT, ID3D12Pageable *const *)
{
    return S_OK;
}

HRESULT Device::GetDeviceRemovedReason()
{
    return S_OK;
}

LUID Device::GetAdapterLuid()
{
    return {static_cast<ULONG>(caps_.registry_id), static_cast<LONG>(caps_.registry_id >> 32)};
}

// ---- Capabilities ----------------------------------------------------------

HRESULT Device::CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size)
{
    if (!data)
        return E_INVALIDARG;
    switch (feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
        o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
        o->TypedUAVLoadAdditionalFormats = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS1>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->WaveOps = TRUE;
        o->WaveLaneCountMin = 32;
        o->WaveLaneCountMax = 32;
        o->TotalLaneCount = 32;
        o->ExpandedComputeResourceStates = TRUE;
        o->Int64ShaderOps = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
        auto *a = feature_data<D3D12_FEATURE_DATA_ARCHITECTURE>(data, size);
        if (!a)
            return E_INVALIDARG;
        if (a->NodeIndex != 0)
            return E_INVALIDARG;
        a->TileBasedRenderer = TRUE;
        a->UMA = TRUE;
        a->CacheCoherentUMA = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
        auto *a = feature_data<D3D12_FEATURE_DATA_ARCHITECTURE1>(data, size);
        if (!a)
            return E_INVALIDARG;
        if (a->NodeIndex != 0)
            return E_INVALIDARG;
        a->TileBasedRenderer = TRUE;
        a->UMA = TRUE;
        a->CacheCoherentUMA = TRUE;
        a->IsolatedMMU = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
        auto *f = feature_data<D3D12_FEATURE_DATA_FEATURE_LEVELS>(data, size);
        if (!f || !f->pFeatureLevelsRequested)
            return E_INVALIDARG;
        D3D_FEATURE_LEVEL best = {};
        for (UINT i = 0; i < f->NumFeatureLevels; ++i) {
            D3D_FEATURE_LEVEL level = f->pFeatureLevelsRequested[i];
            if (level <= kMaxFeatureLevel && level > best)
                best = level;
        }
        if (!best)
            return E_INVALIDARG;
        f->MaxSupportedFeatureLevel = best;
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
        auto *f = feature_data<D3D12_FEATURE_DATA_FORMAT_SUPPORT>(data, size);
        if (!f)
            return E_INVALIDARG;
        mtlb_format_info info;
        f->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
        f->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        if (get_format_info(f->Format, &info)) {
            f->Support1 = format_support1(info);
            f->Support2 = format_support2(info);
        }
        return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
        auto *f = feature_data<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS>(data, size);
        if (!f)
            return E_INVALIDARG;
        mtlb_format_info info;
        const bool supported = get_format_info(f->Format, &info) && (info.flags & MTLB_FORMAT_FLAG_RENDER_TARGET)
                               && (f->SampleCount == 1 || f->SampleCount == 2 || f->SampleCount == 4 || f->SampleCount == 8);
        f->NumQualityLevels = supported ? 1 : 0;
        return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
        auto *f = feature_data<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT>(data, size);
        if (!f)
            return E_INVALIDARG;
        f->MaxGPUVirtualAddressBitsPerResource = 40;
        f->MaxGPUVirtualAddressBitsPerProcess = 40;
        return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
        auto *f = feature_data<D3D12_FEATURE_DATA_SHADER_MODEL>(data, size);
        if (!f)
            return E_INVALIDARG;
        f->HighestShaderModel = std::min(f->HighestShaderModel, kMaxShaderModel);
        return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
        auto *f = feature_data<D3D12_FEATURE_DATA_ROOT_SIGNATURE>(data, size);
        if (!f)
            return E_INVALIDARG;
        f->HighestVersion = std::min(f->HighestVersion, D3D_ROOT_SIGNATURE_VERSION_1_1);
        return S_OK;
    }
    default:
        D3D12M_LOG("CheckFeatureSupport: feature %d is not implemented", static_cast<int>(feature));
        return E_INVALIDARG;
    }
}

} // namespace d3d12m
