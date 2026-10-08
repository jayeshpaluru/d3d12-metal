#include "d3d12/command_stream.h"
#include "d3d12/device.h"
#include "d3d12/meta_command.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

#include "common/config.h"
#include "common/luid.h"
#include "common/platform.h"
#include "common/stats.h"
#include "d3d12/command_allocator.h"
#include "d3d12/command_list.h"
#include "d3d12/command_queue.h"
#include "d3d12/command_signature.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/dred.h"
#include "d3d12/fence.h"
#include "d3d12/formats.h"
#include "d3d12/heap.h"
#include "d3d12/pipeline_state.h"
#include "d3d12/query_heap.h"
#include "d3d12/resource.h"
#include "dxgi/dxgi_interfaces.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

D3D_FEATURE_LEVEL max_feature_level()
{
    static const D3D_FEATURE_LEVEL level = [] {
        const char *value = config_get("FEATURE_LEVEL");
        if (!value || !*value || !std::strcmp(value, "12_0"))
            return D3D_FEATURE_LEVEL_12_0;
        if (!std::strcmp(value, "12_1")) {
            D3D12M_LOG("feature_level=12_1: reporting feature level 12_1 and rasterizer ordered views; conservative "
                       "rasterization stays unsupported (a test switch, not a conformant 12_1)");
            return D3D_FEATURE_LEVEL_12_1;
        }
        D3D12M_LOG("feature_level=%s is not supported (12_0 or 12_1); using 12_0", value);
        return D3D_FEATURE_LEVEL_12_0;
    }();
    return level;
}

namespace {

constexpr D3D_SHADER_MODEL kMaxShaderModel = D3D_SHADER_MODEL_6_6;
// D3D12_FEATURE values newer than the MinGW headers.
constexpr int kFeatureOptions19 = 48;
// Newer feature queries (Agility SDK headers the MinGW build lacks): all answered "not supported".
constexpr int kFeaturePredication = 50;
constexpr int kFeaturePlacedResourceSupportInfo = 51;
constexpr int kFeatureHardwareCopy = 52;
constexpr int kFeatureTightAlignment = 54;
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
    mtlb_cache_configure(platform_executable_name().c_str());
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
    D3D12M_TRACE();
    return 1;
}

HRESULT Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? CommandQueue::create(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return CommandAllocator::create(this, type, riid, out);
    D3D12M_TRACED_END(type, riid, out)
}

HRESULT Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? PipelineState::create_graphics(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? PipelineState::create_compute(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? PipelineState::create_from_stream(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable *const *, ID3D12Fence *fence, UINT64 value)
{
    D3D12M_TRACED_BEGIN
    // Everything is resident, so the "enqueued" work is complete at once.
    return fence ? fence->Signal(value) : E_INVALIDARG;
    D3D12M_TRACED_END(fence, value)
}

HRESULT Device::CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
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
    D3D12M_TRACED_END(type, riid, out)
}

HRESULT Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return CommandList::create(this, type, allocator, initial_state, riid, out);
    D3D12M_TRACED_END(type, allocator, initial_state, riid, out)
}

HRESULT Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? DescriptorHeap::create(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

UINT Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12M_TRACE(type);
    return descriptor_size(type);
}

HRESULT Device::CreateRootSignature(UINT, const void *blob, SIZE_T size, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return RootSignature::create(this, blob, size, riid, out);
    D3D12M_TRACED_END(blob, size, riid, out)
}

HRESULT Device::CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return Fence::create(this, initial_value, flags, riid, out);
    D3D12M_TRACED_END(initial_value, flags, riid, out)
}

HRESULT Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? QueryHeap::create(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root_signature,
                                       REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? CommandSignature::create(this, *desc, root_signature, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, root_signature, riid, out)
}

HRESULT Device::CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return desc ? Heap::create(this, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(desc, riid, out)
}

HRESULT Device::CreatePlacedResource(ID3D12Heap *heap_ptr, UINT64 offset, const D3D12_RESOURCE_DESC *desc,
                                     D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    auto *heap = ours<Heap>(heap_ptr);
    if (!heap || !desc)
        return E_INVALIDARG;
    return Resource::create_placed(this, heap, offset, *desc, riid, out);
    D3D12M_TRACED_END(heap_ptr, offset, desc, riid, out)
}

HRESULT Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *desc,
                                        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    return heap && desc ? Resource::create_committed(this, *heap, *desc, riid, out) : E_INVALIDARG;
    D3D12M_TRACED_END(heap, desc, riid, out)
}

HRESULT Device::CreateCommittedResource2(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                         D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                                         ID3D12ProtectedResourceSession *, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreateCommittedResource(heap, flags, &plain, state, clear, riid, out);
    D3D12M_TRACED_END(heap, flags, desc, state, clear, riid, out)
}

HRESULT Device::CreatePlacedResource1(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreatePlacedResource(heap, offset, &plain, state, clear, riid, out);
    D3D12M_TRACED_END(heap, offset, desc, state, clear, riid, out)
}

// Enhanced barriers are not supported; the layout only says how the resource starts, which this layer does not track.
HRESULT Device::CreateCommittedResource3(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                                         D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *,
                                         UINT32, D3D12M_CASTABLE_FORMATS, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreateCommittedResource(heap, flags, &plain, D3D12_RESOURCE_STATE_COMMON, clear, riid, out);
    D3D12M_TRACED_END(heap, flags, desc, clear, riid, out)
}

HRESULT Device::CreatePlacedResource2(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT,
                                      const D3D12_CLEAR_VALUE *clear, UINT32, D3D12M_CASTABLE_FORMATS, REFIID riid, void **out)
{
    D3D12M_TRACED_BEGIN
    if (!desc)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC plain = to_desc(*desc);
    return CreatePlacedResource(heap, offset, &plain, D3D12_RESOURCE_STATE_COMMON, clear, riid, out);
    D3D12M_TRACED_END(heap, offset, desc, clear, riid, out)
}

void Device::GetCopyableFootprints1(const D3D12_RESOURCE_DESC1 *desc, UINT first, UINT count, UINT64 base_offset,
                                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows, UINT64 *row_sizes,
                                    UINT64 *total_bytes)
{
    D3D12M_TRACE(desc, first, count, base_offset, layouts, num_rows, row_sizes, total_bytes);
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

// Generations are unique across devices, so a thread-local heap cache can never match a device (or a heap)
// that reuses the address of an older one.
static std::atomic<uint64_t> g_heap_generations{1};

void Device::register_heap(DescriptorHeap *heap)
{
    if (!heap->storage())
        return;
    std::unique_lock lock(heaps_mutex_);
    heaps_[reinterpret_cast<uintptr_t>(heap->storage())] = heap;
    heap_generation_.store(++g_heap_generations, std::memory_order_release);
}

void Device::unregister_heap(DescriptorHeap *heap)
{
    if (!heap->storage())
        return;
    std::unique_lock lock(heaps_mutex_);
    heaps_.erase(reinterpret_cast<uintptr_t>(heap->storage()));
    heap_generation_.store(++g_heap_generations, std::memory_order_release);
}

DescriptorHeap *Device::validate_cpu_range(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT count, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           size_t *index)
{
    const uint64_t generation = heap_generation_.load(std::memory_order_acquire);
    DescriptorHeap *heap = nullptr;
    uintptr_t begin = 0, end = 0;
    // Copies alternate between a destination and a source heap, so a few slots are tried, the last hit first.
    const unsigned hint = heap_slot_hint_.load(std::memory_order_relaxed);
    for (unsigned n = 0; n < kHeapSlots && !heap; ++n) {
        const unsigned i = (hint + n) % kHeapSlots;
        HeapSlot &slot = heap_slots_[i];
        const uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if (before & 1)
            continue;
        const uint64_t slot_generation = slot.generation.load(std::memory_order_relaxed);
        const uintptr_t slot_begin = slot.begin.load(std::memory_order_relaxed), slot_end = slot.end.load(std::memory_order_relaxed);
        DescriptorHeap *slot_heap = slot.heap.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) != before || slot_generation != generation || handle.ptr < slot_begin
            || handle.ptr >= slot_end)
            continue;
        heap = slot_heap;
        begin = slot_begin;
        end = slot_end;
        if (i != hint)
            heap_slot_hint_.store(i, std::memory_order_relaxed);
    }
    if (!heap) {
        {
            std::shared_lock lock(heaps_mutex_);
            auto it = heaps_.upper_bound(handle.ptr);
            if (it == heaps_.begin())
                return nullptr;
            --it;
            heap = it->second;
            begin = it->first;
            end = it->first + size_t(heap->count()) * kDescriptorSize;
            if (handle.ptr >= end)
                return nullptr;
        }
        // Remember it, unless a heap changed meanwhile (the generation then no longer matches and the next query looks again).
        std::lock_guard<std::mutex> writers(heap_slot_mutex_);
        const unsigned i = heap_slot_next_.fetch_add(1, std::memory_order_relaxed) % kHeapSlots;
        HeapSlot &slot = heap_slots_[i];
        const uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
        slot.sequence.store(sequence + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        slot.generation.store(generation, std::memory_order_relaxed);
        slot.begin.store(begin, std::memory_order_relaxed);
        slot.end.store(end, std::memory_order_relaxed);
        slot.heap.store(heap, std::memory_order_relaxed);
        slot.sequence.store(sequence + 2, std::memory_order_release);
        heap_slot_hint_.store(i, std::memory_order_relaxed);
    }
    const size_t offset = handle.ptr - begin;
    if (heap->type() != type || offset % kDescriptorSize != 0 || size_t(count) * kDescriptorSize > end - handle.ptr)
        return nullptr;
    if (index)
        *index = offset / kDescriptorSize;
    return heap;
}

uint64_t Device::register_attachment(Resource *resource)
{
    const uint64_t id = next_attachment_id_++;
    std::unique_lock lock(attachments_mutex_);
    attachments_[id] = resource;
    return id;
}

void Device::unregister_attachment(uint64_t id)
{
    std::unique_lock lock(attachments_mutex_);
    attachments_.erase(id);
}

Resource *Device::acquire_attachment(uint64_t id)
{
    std::shared_lock lock(attachments_mutex_);
    auto it = attachments_.find(id);
    // A resource whose last reference is gone is being destroyed (its destructor waits for this lock).
    return it != attachments_.end() && it->second->try_add_internal_ref() ? it->second : nullptr;
}

// The CBV/SRV/UAV heap that holds a CPU handle and the index of the handle in it. The caller holds heaps_mutex_.
DescriptorHeap *Device::locate_view_heap(D3D12_CPU_DESCRIPTOR_HANDLE handle, size_t *index) const
{
    auto it = heaps_.upper_bound(handle.ptr);
    if (it == heaps_.begin())
        return nullptr;
    --it;
    DescriptorHeap *heap = it->second;
    if (heap->type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        return nullptr;
    *index = (handle.ptr - it->first) / kDescriptorSize;
    return heap;
}

ViewInfo *Device::view_info(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    std::shared_lock lock(heaps_mutex_);
    size_t index;
    DescriptorHeap *heap = locate_view_heap(handle, &index);
    return heap ? heap->shadow(static_cast<uint32_t>(index)) : nullptr;
}

// Copies the shadow info of `count` descriptors between heaps that were already located; overlapping ranges of one
// heap behave like memmove.
void Device::copy_view_info(DescriptorHeap *to_heap, size_t to_index, DescriptorHeap *from_heap, size_t from_index, UINT count)
{
    ViewInfo *to = to_heap->shadow(static_cast<uint32_t>(to_index));
    ViewInfo *from = from_heap->shadow(static_cast<uint32_t>(from_index));
    if (to && from)
        std::memmove(static_cast<void *>(to), static_cast<const void *>(from), size_t(count) * sizeof(ViewInfo));
}

// ---- Forced initial state of placed render targets and depth-stencils ---------

void Device::add_pending_init(Resource *resource)
{
    std::lock_guard<std::mutex> lock(init_mutex_);
    pending_init_.push_back(resource);
}

void Device::remove_pending_init(Resource *resource)
{
    std::lock_guard<std::mutex> lock(init_mutex_);
    pending_init_.erase(std::remove(pending_init_.begin(), pending_init_.end(), resource), pending_init_.end());
}

std::vector<Resource *> Device::take_pending_init(std::vector<uint8_t> &stream)
{
    std::vector<Resource *> held;
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        // A resource whose last reference went away is being destroyed (its destructor waits for this lock):
        // nothing to initialise, and it must not be resurrected.
        for (Resource *resource : pending_init_) {
            if (resource->try_add_internal_ref())  // keeps it alive until the submission is done
                held.push_back(resource);
        }
        pending_init_.clear();
    }
    if (held.empty())
        return held;
    append_record<mtlb_cmd_reset_state>(stream, MTLB_CMD_RESET_STATE);
    for (Resource *resource : held) {
        const D3D12_RESOURCE_DESC &desc = resource->desc();
        const bool depth = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const UINT mips = desc.MipLevels;
        for (UINT mip = 0; mip < mips; ++mip) {
            const UINT slices = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(desc.DepthOrArraySize, mip)
                                                                                     : array_size(desc);
            for (UINT slice = 0; slice < slices; ++slice) {
                const mtlb_render_target target = {resource->texture(), 0, mip, slice, 0};
                if (depth) {
                    auto *cmd = append_record<mtlb_cmd_clear_dsv>(stream, MTLB_CMD_CLEAR_DSV);
                    cmd->target = target;
                    cmd->flags = MTLB_CLEAR_DEPTH | MTLB_CLEAR_STENCIL;
                } else {
                    auto *cmd = append_record<mtlb_cmd_clear_rtv>(stream, MTLB_CMD_CLEAR_RTV);
                    cmd->target = target;
                }
            }
        }
    }
    return held;
}

// ---- Descriptors -----------------------------------------------------------

// A descriptor write whose handle is not inside a heap of the right type is dropped (logged once): the
// application passed a stale or foreign handle, and writing there would corrupt memory.
void log_bad_handle(const char *what)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        D3D12M_LOG("%s: the descriptor handle is not inside a descriptor heap of the right type (further cases are not logged)", what);
}

namespace {

// A view of this mip level and slice must exist (a depth plane for 3D textures).
bool attachment_range_valid(const D3D12_RESOURCE_DESC &rd, UINT mip, UINT slice)
{
    if (mip >= resolve_mip_levels(rd))
        return false;
    const UINT slices = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(rd.DepthOrArraySize, mip) : array_size(rd);
    return slice < slices;
}

} // namespace

void Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12M_TRACE(desc, dest);
    if (!validate_cpu_range(dest, 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV))
        return log_bad_handle("CreateConstantBufferView");
    stat_add(Stat::DescriptorWrites);
    auto *entry = reinterpret_cast<mtlb_descriptor *>(dest.ptr);
    if (desc)
        mtlb_descriptor_set_buffer(entry, desc->BufferLocation, desc->SizeInBytes);
    else
        *entry = {};
}

// Shared by RTVs and DSVs: the slot gets the resource id and the view's format, mip and slice when the view
// is valid, and stays a null view otherwise.
static void fill_attachment(Resource *resource, D3D12_RESOURCE_FLAGS required, DXGI_FORMAT format, UINT mip, UINT slice,
                            RenderTargetDescriptor *slot)
{
    *slot = {};
    if (!resource || resource->is_buffer() || !resource->attachment_id() || !(resource->desc().Flags & required))
        return;
    if (!attachment_range_valid(resource->desc(), mip, slice)) {
        D3D12M_LOG("render target or depth-stencil view of mip %u, slice %u: the resource has no such subresource", mip, slice);
        return;
    }
    slot->resource_id = resource->attachment_id();
    slot->mip_level = mip;
    slot->array_slice = slice;
    // A view of a different format than the texture's own needs a texture view;
    // DXGI_FORMAT_UNKNOWN means the texture's format.
    if (format != DXGI_FORMAT_UNKNOWN && to_mtlb_format(format) != to_mtlb_format(resource->desc().Format))
        slot->view_format = to_mtlb_format(format);
}

void Device::CreateRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12M_TRACE(resource, desc, dest);
    if (!validate_cpu_range(dest, 1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
        return log_bad_handle("CreateRenderTargetView");
    stat_add(Stat::DescriptorWrites);
    auto *slot = reinterpret_cast<RenderTargetDescriptor *>(dest.ptr);
    // One mip level and one slice (a layered view renders to its first slice).
    UINT mip = 0, slice = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (desc) {
        format = desc->Format;
        switch (desc->ViewDimension) {
        case D3D12_RTV_DIMENSION_TEXTURE1D: mip = desc->Texture1D.MipSlice; break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            mip = desc->Texture1DArray.MipSlice;
            slice = desc->Texture1DArray.FirstArraySlice;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2D: mip = desc->Texture2D.MipSlice; break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            mip = desc->Texture2DArray.MipSlice;
            slice = desc->Texture2DArray.FirstArraySlice;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: slice = desc->Texture2DMSArray.FirstArraySlice; break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            mip = desc->Texture3D.MipSlice;
            slice = desc->Texture3D.FirstWSlice;
            break;
        default: break;
        }
    }
    fill_attachment(ours<Resource>(resource), D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, format, mip, slice, slot);
}

void Device::CreateDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12M_TRACE(resource, desc, dest);
    if (!validate_cpu_range(dest, 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
        return log_bad_handle("CreateDepthStencilView");
    stat_add(Stat::DescriptorWrites);
    auto *slot = reinterpret_cast<RenderTargetDescriptor *>(dest.ptr);
    UINT mip = 0, slice = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t flags = 0;
    if (desc) {
        format = desc->Format;
        if (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)
            flags |= MTLB_DEPTH_READ_ONLY;
        if (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)
            flags |= MTLB_STENCIL_READ_ONLY;
        switch (desc->ViewDimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D: mip = desc->Texture1D.MipSlice; break;
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
            mip = desc->Texture1DArray.MipSlice;
            slice = desc->Texture1DArray.FirstArraySlice;
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2D: mip = desc->Texture2D.MipSlice; break;
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            mip = desc->Texture2DArray.MipSlice;
            slice = desc->Texture2DArray.FirstArraySlice;
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY: slice = desc->Texture2DMSArray.FirstArraySlice; break;
        default: break;
        }
    }
    fill_attachment(ours<Resource>(resource), D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, format, mip, slice, slot);
    if (slot->resource_id)
        slot->flags = flags;
}

// All descriptor types use same-sized slots, so copying is a memcpy per range. Ranges outside the heaps (of
// the right type) are skipped.
void Device::CopyDescriptors(UINT num_dest_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_starts,
                             const UINT *dest_sizes, UINT num_src_ranges,
                             const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes,
                             D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12M_TRACE(num_dest_ranges, dest_starts, dest_sizes, num_src_ranges, src_starts, src_sizes, type);
    const size_t size = descriptor_size(type);
    if (!size || !dest_starts || !src_starts)
        return;
    UINT d = 0, s = 0, d_used = 0, s_used = 0;
    while (d < num_dest_ranges && s < num_src_ranges) {
        const UINT dest_size = dest_sizes ? dest_sizes[d] : 1;
        const UINT src_size = src_sizes ? src_sizes[s] : 1;
        const UINT n = std::min(dest_size - d_used, src_size - s_used);
        const D3D12_CPU_DESCRIPTOR_HANDLE to = {dest_starts[d].ptr + size_t(d_used) * size};
        const D3D12_CPU_DESCRIPTOR_HANDLE from = {src_starts[s].ptr + size_t(s_used) * size};
        size_t to_index = 0, from_index = 0;
        DescriptorHeap *to_heap = n ? validate_cpu_range(to, n, type, &to_index) : nullptr;
        DescriptorHeap *from_heap = to_heap ? validate_cpu_range(from, n, type, &from_index) : nullptr;
        if (from_heap) {
            std::memmove(reinterpret_cast<void *>(to.ptr), reinterpret_cast<const void *>(from.ptr), size_t(n) * size);
            if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                copy_view_info(to_heap, to_index, from_heap, from_index, n);
            stat_add(Stat::DescriptorWrites, n);
        } else if (n) {
            log_bad_handle("CopyDescriptors");
        }
        d_used += n;
        s_used += n;
        if (d_used == dest_size) { ++d; d_used = 0; }
        if (s_used == src_size) { ++s; s_used = 0; }
    }
}

void Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12M_TRACE(count, dest, src, type);
    const size_t size = descriptor_size(type);
    if (!size || !count)
        return;
    size_t to_index = 0, from_index = 0;
    DescriptorHeap *to_heap = validate_cpu_range(dest, count, type, &to_index);
    DescriptorHeap *from_heap = to_heap ? validate_cpu_range(src, count, type, &from_index) : nullptr;
    if (!from_heap)
        return log_bad_handle("CopyDescriptorsSimple");
    std::memmove(reinterpret_cast<void *>(dest.ptr), reinterpret_cast<const void *>(src.ptr), size_t(count) * size);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        copy_view_info(to_heap, to_index, from_heap, from_index, count);
    stat_add(Stat::DescriptorWrites, count);
}

// ---- Resources -------------------------------------------------------------

// The size and alignment a resource takes in a heap, as Metal places it.
bool Device::resource_size_align(const D3D12_RESOURCE_DESC &desc, mtlb_size_align *out) const
{
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return desc.Width != 0 && mtlb_buffer_size_align(device_, desc.Width, MTLB_STORAGE_PRIVATE, out) == MTLB_OK;
    mtlb_texture_desc td;
    return to_texture_desc(desc, MTLB_STORAGE_PRIVATE, &td) && mtlb_texture_size_align(device_, &td, out) == MTLB_OK;
}

// Resources laid out one after the other, each at the alignment it needs (at least 64 KB, D3D12's placement
// alignment).
D3D12_RESOURCE_ALLOCATION_INFO Device::allocation_info(UINT count, const D3D12_RESOURCE_DESC *descs,
                                                       D3D12_RESOURCE_ALLOCATION_INFO1 *info1) const
{
    D3D12_RESOURCE_ALLOCATION_INFO info{0, kResourceAlignment};
    for (UINT i = 0; i < count; ++i) {
        mtlb_size_align size_align;
        if (!resource_size_align(descs[i], &size_align))
            return {UINT64_MAX, kResourceAlignment};
        const UINT64 alignment = std::max<UINT64>(kResourceAlignment, size_align.align);
        const UINT64 offset = align_up(info.SizeInBytes, alignment);
        const UINT64 size = align_up(size_align.size, kResourceAlignment);
        if (info1)
            info1[i] = {offset, alignment, size};
        info.SizeInBytes = offset + size;
        info.Alignment = std::max(info.Alignment, alignment);
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
    D3D12M_TRACE(desc, first, count, base_offset, layouts, num_rows, row_sizes, total_bytes);
    if (!compute_copyable_footprints(*desc, first, count, base_offset, layouts, num_rows, row_sizes, total_bytes))
        D3D12M_LOG("GetCopyableFootprints: unsupported format or subresource range");
}

HRESULT Device::MakeResident(UINT, ID3D12Pageable *const *)
{
    D3D12M_TRACED_BEGIN
    return S_OK;  // every allocation is always resident
    D3D12M_TRACED_END()
}

HRESULT Device::Evict(UINT, ID3D12Pageable *const *)
{
    D3D12M_TRACED_BEGIN
    return S_OK;
    D3D12M_TRACED_END()
}

bool Device::failed_pipeline(uint64_t key, HRESULT *hr)
{
    std::lock_guard<std::mutex> lock(failed_pipelines_mutex_);
    const auto it = failed_pipelines_.find(key);
    if (it == failed_pipelines_.end())
        return false;
    *hr = it->second;
    return true;
}

bool Device::note_failed_pipeline(uint64_t key, HRESULT hr)
{
    std::lock_guard<std::mutex> lock(failed_pipelines_mutex_);
    return failed_pipelines_.emplace(key, hr).second;
}

RootSignature *Device::find_embedded_root_signature(const RootSignatureKey &key)
{
    std::lock_guard<std::mutex> lock(embedded_signatures_mutex_);
    for (RootSignature *signature : embedded_signatures_) {
        // One that is being destroyed refuses the reference and is skipped (it removes itself right after).
        if (signature->content_key() == key && signature->try_add_internal_ref())
            return signature;
    }
    return nullptr;
}

void Device::add_embedded_root_signature(RootSignature *signature)
{
    std::lock_guard<std::mutex> lock(embedded_signatures_mutex_);
    embedded_signatures_.push_back(signature);
}

void Device::remove_embedded_root_signature(RootSignature *signature)
{
    std::lock_guard<std::mutex> lock(embedded_signatures_mutex_);
    embedded_signatures_.erase(std::remove(embedded_signatures_.begin(), embedded_signatures_.end(), signature),
                               embedded_signatures_.end());
}

HRESULT Device::GetDeviceRemovedReason()
{
    D3D12M_TRACED_BEGIN
    return S_OK;
    D3D12M_TRACED_END()
}

LUID Device::adapter_luid() const
{
    return luid_from_registry_id(caps_.registry_id);
}

// ---- Capabilities ----------------------------------------------------------

// What the layer reports is what the backend can honor: see docs/STATUS.md.
HRESULT Device::CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size)
{
    D3D12M_TRACED_BEGIN
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
        // Rasterizer ordered views are backed by the converter (raster order groups; tests/portable/p_rov.cpp) but only
        // reported at the feature level that requires them, so the default behaviour of titles stays what it was.
        o->ROVsSupported = max_feature_level() >= D3D_FEATURE_LEVEL_12_1;
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
    case kFeaturePredication:
    case kFeaturePlacedResourceSupportInfo:
    case kFeatureHardwareCopy:
    case kFeatureTightAlignment:
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
            if (level <= max_feature_level() && level > best)
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
    case D3D12_FEATURE_QUERY_META_COMMAND: {
        auto *f = feature_data<D3D12_FEATURE_DATA_QUERY_META_COMMAND>(data, size);
        return f ? query_meta_command(*f) : E_INVALIDARG;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
        auto *f = feature_data<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY>(data, size);
        if (!f)
            return E_INVALIDARG;
        f->PriorityForTypeIsSupported = supported_queue_type(f->CommandListType)
                                        && (f->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL
                                            || f->Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        return S_OK;
    }
    default:
        D3D12M_LOG("CheckFeatureSupport: feature %d is not implemented", static_cast<int>(feature));
        return E_INVALIDARG;
    }
    D3D12M_TRACED_END(feature, data, size)
}

} // namespace d3d12m
