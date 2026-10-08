#include "d3d12/device.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

#include "common/luid.h"
#include "d3d12/command_allocator.h"
#include "d3d12/command_list.h"
#include "d3d12/command_queue.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/dred.h"
#include "d3d12/fence.h"
#include "d3d12/formats.h"
#include "d3d12/pipeline_state.h"
#include "d3d12/resource.h"
#include "dxgi/dxgi_interfaces.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

namespace {

constexpr D3D_FEATURE_LEVEL kMaxFeatureLevel = D3D_FEATURE_LEVEL_12_0;
constexpr D3D_SHADER_MODEL kMaxShaderModel = D3D_SHADER_MODEL_6_6;
// D3D12_FEATURE values newer than the MinGW headers.
constexpr int kFeatureOptions19 = 48;
constexpr int kFeatureOptions20 = 49;
constexpr int kFeatureOptions21 = 53;
constexpr int kFeatureOptions22 = 65;
constexpr UINT64 kResourceAlignment = 64 * 1024;

// Typed access to the in/out structure of CheckFeatureSupport.
template <typename T>
T *feature_data(void *data, UINT size)
{
    return size == sizeof(T) ? static_cast<T *>(data) : nullptr;
}

// Answers D3D12_FEATURE_ARCHITECTURE and ARCHITECTURE1: a single UMA, tile-based GPU.
template <typename T>
HRESULT fill_architecture(void *data, UINT size)
{
    auto *a = feature_data<T>(data, size);
    if (!a || a->NodeIndex != 0)
        return E_INVALIDARG;
    a->TileBasedRenderer = TRUE;
    a->UMA = TRUE;
    a->CacheCoherentUMA = TRUE;
    if constexpr (std::is_same_v<T, D3D12_FEATURE_DATA_ARCHITECTURE1>)
        a->IsolatedMMU = TRUE;
    return S_OK;
}

D3D12_FORMAT_SUPPORT1 format_support1(DXGI_FORMAT format, const mtlb_format_info &info)
{
    UINT support = 0;
    const bool depth = info.flags & MTLB_FORMAT_FLAG_DEPTH;
    if (info.flags & MTLB_FORMAT_FLAG_TEXTURE) {
        support |= D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE
                   | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE
                   | D3D12_FORMAT_SUPPORT1_MIP | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
        if (!depth && !(info.flags & MTLB_FORMAT_FLAG_COMPRESSED))
            support |= D3D12_FORMAT_SUPPORT1_SHADER_GATHER | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
        if (depth)
            support |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;
        // 1D textures are 2D textures of height one; block-compressed formats have no 1D or 3D form on D3D12 either.
        if (!(info.flags & MTLB_FORMAT_FLAG_COMPRESSED) && !depth)
            support |= D3D12_FORMAT_SUPPORT1_TEXTURE1D | D3D12_FORMAT_SUPPORT1_TEXTURE3D;
        if (info.flags & MTLB_FORMAT_FLAG_COMPRESSED)
            support |= D3D12_FORMAT_SUPPORT1_TEXTURE3D;
    }
    if (info.flags & MTLB_FORMAT_FLAG_RENDER_TARGET) {
        if (depth)
            support |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        else
            support |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET
                       | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
    }
    if (info.flags & MTLB_FORMAT_FLAG_BLENDABLE)
        support |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
    if (info.flags & MTLB_FORMAT_FLAG_VERTEX)
        support |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
    if (format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT)
        support |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;
    if (info.flags & MTLB_FORMAT_FLAG_SHADER_WRITE)
        support |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    if (info.flags & MTLB_FORMAT_FLAG_BUFFER)
        support |= D3D12_FORMAT_SUPPORT1_BUFFER;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM
        || format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        support |= D3D12_FORMAT_SUPPORT1_DISPLAY;
    return static_cast<D3D12_FORMAT_SUPPORT1>(support);
}

D3D12_FORMAT_SUPPORT2 format_support2(DXGI_FORMAT format, const mtlb_format_info &info)
{
    UINT support = 0;
    if (info.flags & MTLB_FORMAT_FLAG_SHADER_WRITE)
        support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if (format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R32_SINT) {
        support |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS
                   | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE
                   | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX
                   | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
    }
    return static_cast<D3D12_FORMAT_SUPPORT2>(support);
}

D3D12_RESOURCE_DESC to_desc(const D3D12_RESOURCE_DESC1 &d)
{
    return {d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.SampleDesc,
            d.Layout, d.Flags};
}

// Zero-fills a feature structure the layer answers with "unsupported".
HRESULT unsupported_feature(void *data, UINT size)
{
    std::memset(data, 0, size);
    return S_OK;
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

HRESULT Device::create(IUnknown *adapter, ID3D12Device10 **out)
{
    uint64_t registry_id = 0;
    if (adapter) {
        IDXGIAdapter *dxgi_adapter = nullptr;
        DXGI_ADAPTER_DESC desc;
        if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(&dxgi_adapter))))
            return E_INVALIDARG;
        HRESULT hr = dxgi_adapter->GetDesc(&desc);
        dxgi_adapter->Release();
        if (FAILED(hr))
            return hr;
        registry_id = registry_id_from_luid(desc.AdapterLuid);
    }

    auto *device = new Device();
    if (mtlb_device_create(registry_id, &device->device_) != MTLB_OK || mtlb_device_get_caps(device->device_, &device->caps_) != MTLB_OK) {
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

HRESULT Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    return desc ? PipelineState::create_compute(this, *desc, riid, out) : E_INVALIDARG;
}

HRESULT Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
    return desc ? PipelineState::create_from_stream(this, *desc, riid, out) : E_INVALIDARG;
}

HRESULT Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable *const *, ID3D12Fence *fence, UINT64 value)
{
    // Everything is resident, so the "enqueued" work is complete at once.
    return fence ? fence->Signal(value) : E_INVALIDARG;
}

HRESULT Device::CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    ID3D12CommandAllocator *allocator = nullptr;
    HRESULT hr = CommandAllocator::create(this, type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&allocator));
    if (FAILED(hr))
        return hr;
    ID3D12GraphicsCommandList *list = nullptr;
    hr = CommandList::create(this, type, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&list));
    allocator->Release();
    if (FAILED(hr))
        return hr;
    hr = list->Close();  // created closed
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(riid, out);
    list->Release();
    return hr;
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

UINT Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    return descriptor_size(type);
}

HRESULT Device::CreateRootSignature(UINT, const void *blob, SIZE_T size, REFIID riid, void **out)
{
    return RootSignature::create(this, blob, size, riid, out);
}

HRESULT Device::CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **out)
{
    return Fence::create(this, initial_value, flags, riid, out);
}

HRESULT Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *desc,
                                        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out)
{
    return heap && desc ? Resource::create_committed(this, *heap, *desc, riid, out) : E_INVALIDARG;
}

HRESULT Device::CreateCommittedResource2(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                         D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                                         ID3D12ProtectedResourceSession *, REFIID riid, void **out)
{
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreateCommittedResource(heap, flags, &plain, state, clear, riid, out);
}

HRESULT Device::CreatePlacedResource1(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreatePlacedResource(heap, offset, &plain, state, clear, riid, out);
}

// Enhanced barriers are not supported; the layout only says how the resource starts, which this layer does not track.
HRESULT Device::CreateCommittedResource3(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                         D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *,
                                         UINT32, D3D12M_CASTABLE_FORMATS, REFIID riid, void **out)
{
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreateCommittedResource(heap, flags, &plain, D3D12_RESOURCE_STATE_COMMON, clear, riid, out);
}

HRESULT Device::CreatePlacedResource2(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT,
                                      const D3D12_CLEAR_VALUE *clear, UINT32, D3D12M_CASTABLE_FORMATS, REFIID riid, void **out)
{
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreatePlacedResource(heap, offset, &plain, D3D12_RESOURCE_STATE_COMMON, clear, riid, out);
}

void Device::GetCopyableFootprints1(const D3D12_RESOURCE_DESC1 *desc, UINT first, UINT count, UINT64 base_offset,
                                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows, UINT64 *row_sizes,
                                    UINT64 *total_bytes)
{
    if (!desc)
        return;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    GetCopyableFootprints(&plain, first, count, base_offset, layouts, num_rows, row_sizes, total_bytes);
}

HRESULT Device::query_removed_extended_data(REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    return dred_data()->QueryInterface(riid, out);
}

void Device::register_heap(DescriptorHeap *heap)
{
    if (!heap->storage())
        return;
    std::unique_lock lock(heaps_mutex_);
    heaps_[reinterpret_cast<uintptr_t>(heap->storage())] = heap;
}

void Device::unregister_heap(DescriptorHeap *heap)
{
    if (!heap->storage())
        return;
    std::unique_lock lock(heaps_mutex_);
    heaps_.erase(reinterpret_cast<uintptr_t>(heap->storage()));
}

ViewInfo *Device::view_info(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    std::shared_lock lock(heaps_mutex_);
    auto it = heaps_.upper_bound(handle.ptr);
    if (it == heaps_.begin())
        return nullptr;
    --it;
    DescriptorHeap *heap = it->second;
    if (heap->type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        return nullptr;
    const size_t index = (handle.ptr - it->first) / kDescriptorSize;
    return heap->shadow(static_cast<uint32_t>(index));
}

void Device::copy_view_info(D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, UINT count)
{
    for (UINT i = 0; i < count; ++i) {
        ViewInfo *to = view_info({dest.ptr + size_t(i) * kDescriptorSize});
        ViewInfo *from = view_info({src.ptr + size_t(i) * kDescriptorSize});
        if (to)
            *to = from ? *from : ViewInfo{};
    }
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
    auto *texture = ours<Resource>(resource);
    if (!texture || texture->is_buffer())
        return;

    slot->texture = texture->texture();
    if (!desc)
        return;
    // A view of a different format than the texture's own needs a texture view;
    // DXGI_FORMAT_UNKNOWN means the texture's format.
    if (desc->Format != DXGI_FORMAT_UNKNOWN && to_mtlb_format(desc->Format) != to_mtlb_format(texture->desc().Format))
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
                             D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    const size_t size = descriptor_size(type);
    if (!size || !dest_starts || !src_starts)
        return;
    UINT d = 0, s = 0, d_used = 0, s_used = 0;
    while (d < num_dest_ranges && s < num_src_ranges) {
        const UINT dest_size = dest_sizes ? dest_sizes[d] : 1;
        const UINT src_size = src_sizes ? src_sizes[s] : 1;
        const UINT n = std::min(dest_size - d_used, src_size - s_used);
        std::memcpy(reinterpret_cast<void *>(dest_starts[d].ptr + size_t(d_used) * size),
                    reinterpret_cast<const void *>(src_starts[s].ptr + size_t(s_used) * size), size_t(n) * size);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            copy_view_info({dest_starts[d].ptr + size_t(d_used) * size}, {src_starts[s].ptr + size_t(s_used) * size}, n);
        d_used += n;
        s_used += n;
        if (d_used == dest_size) { ++d; d_used = 0; }
        if (s_used == src_size) { ++s; s_used = 0; }
    }
}

void Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    std::memcpy(reinterpret_cast<void *>(dest.ptr), reinterpret_cast<const void *>(src.ptr),
                size_t(count) * descriptor_size(type));
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        copy_view_info(dest, src, count);
}

// ---- Resources -------------------------------------------------------------

D3D12_RESOURCE_ALLOCATION_INFO Device::allocation_info(UINT count, const D3D12_RESOURCE_DESC *descs,
                                                       D3D12_RESOURCE_ALLOCATION_INFO1 *info1) const
{
    D3D12_RESOURCE_ALLOCATION_INFO info{0, kResourceAlignment};
    for (UINT i = 0; i < count; ++i) {
        UINT64 size = 0;
        if (!compute_copyable_footprints(descs[i], 0, subresource_count(descs[i]), 0, nullptr, nullptr, nullptr, &size))
            return {UINT64_MAX, kResourceAlignment};
        const UINT64 aligned = align_up(size, kResourceAlignment);
        if (info1)
            info1[i] = {info.SizeInBytes, kResourceAlignment, aligned};
        info.SizeInBytes += aligned;
    }
    return info;
}

D3D12_RESOURCE_ALLOCATION_INFO Device::allocation_info1(UINT count, const D3D12_RESOURCE_DESC1 *descs,
                                                        D3D12_RESOURCE_ALLOCATION_INFO1 *info1) const
{
    std::vector<D3D12_RESOURCE_DESC> plain;
    plain.reserve(count);
    for (UINT i = 0; i < count; ++i)
        plain.push_back(to_desc(descs[i]));
    return allocation_info(count, plain.data(), info1);
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

LUID Device::adapter_luid() const
{
    return luid_from_registry_id(caps_.registry_id);
}

// ---- Capabilities ----------------------------------------------------------

// What the layer reports is what the backend can honor: see docs/STATUS.md.
HRESULT Device::CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size)
{
    if (!data)
        return E_INVALIDARG;
    switch (static_cast<int>(feature)) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
        o->TypedUAVLoadAdditionalFormats = TRUE;
        o->ROVsSupported = FALSE;
        o->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
        o->MaxGPUVirtualAddressBitsPerResource = 40;
        o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
        o->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
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
        o->TotalLaneCount = 8192;
        o->ExpandedComputeResourceStates = TRUE;
        o->Int64ShaderOps = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS2>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS3>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->CastingFullyTypedFormatSupported = TRUE;
        o->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE
                                              | D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY;
        o->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS4>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
        o->Native16BitShaderOpsSupported = TRUE;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS5>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
        o->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS6: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS7>(data, size);
        if (!o)
            return E_INVALIDARG;
        *o = {};
        o->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        o->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS8:
    case D3D12_FEATURE_D3D12_OPTIONS9:
    case D3D12_FEATURE_D3D12_OPTIONS10:
    case D3D12_FEATURE_D3D12_OPTIONS11:
    case D3D12_FEATURE_D3D12_OPTIONS12:
    case D3D12_FEATURE_D3D12_OPTIONS14:
    case D3D12_FEATURE_D3D12_OPTIONS15:
    case D3D12_FEATURE_D3D12_OPTIONS16:
    case D3D12_FEATURE_D3D12_OPTIONS17:
    case D3D12_FEATURE_D3D12_OPTIONS18:
    case kFeatureOptions19:
    case kFeatureOptions20:
    case kFeatureOptions22:
        // Everything these report is a capability this layer does not have: unaligned block
        // textures, 64-bit typed atomics, mesh shader details, enhanced barriers, triangle fans,
        // dynamic depth bias, GPU upload heaps, non-normalized samplers and so on.
        return unsupported_feature(data, size);
    case D3D12_FEATURE_D3D12_OPTIONS13: {
        // Everything is zero except what Metal does naturally: any copy pitch is allowed.
        unsupported_feature(data, size);
        auto *o = feature_data<D3D12_FEATURE_DATA_D3D12_OPTIONS13>(data, size);
        if (!o)
            return E_INVALIDARG;
        o->UnrestrictedBufferTextureCopyPitchSupported = TRUE;
        return S_OK;
    }
    case kFeatureOptions21: {
        // WorkGraphsTier, ExecuteIndirectTier, SampleCmpGradientAndBiasSupported, ExtendedCommandInfoSupported.
        // The layout is fixed by the header of the Agility SDK; the MinGW headers do not have the struct.
        if (size < 8)
            return E_INVALIDARG;
        std::memset(data, 0, size);
        const UINT32 execute_indirect_tier_1_0 = 10;
        std::memcpy(static_cast<uint8_t *>(data) + 4, &execute_indirect_tier_1_0, 4);
        return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE:
        return fill_architecture<D3D12_FEATURE_DATA_ARCHITECTURE>(data, size);
    case D3D12_FEATURE_ARCHITECTURE1:
        return fill_architecture<D3D12_FEATURE_DATA_ARCHITECTURE1>(data, size);
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
            f->Support1 = format_support1(f->Format, info);
            f->Support2 = format_support2(f->Format, info);
        }
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
        auto *f = feature_data<D3D12_FEATURE_DATA_FORMAT_INFO>(data, size);
        if (!f)
            return E_INVALIDARG;
        mtlb_format_info info;
        if (!get_format_info(f->Format, &info))
            return E_INVALIDARG;
        f->PlaneCount = (info.flags & MTLB_FORMAT_FLAG_DEPTH) && (info.flags & MTLB_FORMAT_FLAG_STENCIL) ? 2 : 1;
        return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
        auto *f = feature_data<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS>(data, size);
        if (!f)
            return E_INVALIDARG;
        mtlb_format_info info;
        const bool known_count = f->SampleCount == 1 || f->SampleCount == 2 || f->SampleCount == 4 || f->SampleCount == 8
                                 || f->SampleCount == 16;
        const bool supported = known_count && get_format_info(f->Format, &info)
                               && (info.flags & MTLB_FORMAT_FLAG_RENDER_TARGET)
                               && (caps_.sample_counts & (1u << f->SampleCount));
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
        // Newer than the highest model is refused, as the runtime does for models it does not know.
        if (static_cast<int>(f->HighestShaderModel) > 0x69)
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
    case D3D12_FEATURE_SHADER_CACHE:
    case D3D12_FEATURE_EXISTING_HEAPS:
    case D3D12_FEATURE_SERIALIZATION:
    case D3D12_FEATURE_CROSS_NODE:
    case D3D12_FEATURE_DISPLAYABLE:
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT:
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
        return unsupported_feature(data, size);
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
        auto *f = feature_data<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY>(data, size);
        if (!f)
            return E_INVALIDARG;
        f->PriorityForTypeIsSupported = supported_list_type(f->CommandListType)
                                        && (f->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL
                                            || f->Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        return S_OK;
    }
    default:
        D3D12M_LOG("CheckFeatureSupport: feature %d is not implemented", static_cast<int>(feature));
        return E_INVALIDARG;
    }
}

} // namespace d3d12m
