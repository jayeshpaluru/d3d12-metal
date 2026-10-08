// ID3D12Device (implemented up to ID3D12Device2).
#pragma once

#include <array>
#include <vector>
#include <map>
#include <mutex>
#include <shared_mutex>

#include "bridge/mtlb.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/fence.h"
#include "d3d12/object.h"

namespace d3d12m {

class Resource;

class Device final : public ObjectImpl<ID3D12Device10> {
public:
    // Creates a device on the Metal device behind `adapter` (an IDXGIAdapter
    // of this layer), or on the system default device when `adapter` is null.
    static HRESULT create(IUnknown *adapter, ID3D12Device10 **out);

    mtlb_device handle() const { return device_; }
    const mtlb_device_caps &caps() const { return caps_; }
    // The descriptor a null view of mtlb_null_kind `kind` gets (cached after the first request).
    mtlb_descriptor null_descriptor(uint32_t kind);

    // Placed render targets and depth-stencils start in a defined state: they are cleared to zero before
    // the first submission after their creation. Resources register at creation (and unregister when
    // destroyed); ExecuteCommandLists takes the pending ones, with a reference each, together with the
    // command stream that clears them.
    void add_pending_init(Resource *resource);
    void remove_pending_init(Resource *resource);
    std::vector<Resource *> take_pending_init(std::vector<uint8_t> &stream);

    // Descriptor heaps by CPU memory, to find the heap behind a CPU descriptor handle.
    void register_heap(DescriptorHeap *heap);
    void unregister_heap(DescriptorHeap *heap);
    // The shadow view info (see ViewInfo) of the CBV/SRV/UAV descriptor at `handle`, or null.
    ViewInfo *view_info(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void copy_view_info(D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, UINT count);
    FenceWaiter &fence_waiter() { return fence_waiter_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(ID3D12DeviceRemovedExtendedData) || riid == __uuidof(ID3D12DeviceRemovedExtendedData1)
            || riid == __uuidof(ID3D12DeviceRemovedExtendedData2))
            return query_removed_extended_data(riid, out);
        return query_interfaces<IUnknown, ID3D12Object, ID3D12Device, ID3D12Device1, ID3D12Device2, ID3D12Device3,
                                ID3D12Device4, ID3D12Device5, ID3D12Device6, ID3D12Device7, ID3D12Device8,
                                ID3D12Device9, ID3D12Device10>(this, riid, out);
    }

    // ID3D12Device
    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *pCommandAllocator, ID3D12PipelineState *pInitialState, REFIID riid, void **ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc, REFIID riid, void **ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void *pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void **ppvRootSignature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts, const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts, const UINT *pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
#ifdef _WIN32
    D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT, UINT count, const D3D12_RESOURCE_DESC *descs) override { *ret = allocation_info(count, descs); return ret; }
    D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *ret, UINT, D3D12_HEAP_TYPE) override { D3D12M_STUB_LOG(); *ret = {}; return ret; }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT count, const D3D12_RESOURCE_DESC *descs) override { return allocation_info(count, descs); }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override { D3D12M_STUB_LOG(); return {}; }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource, void **ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource) override;
    // Tiled (reserved) resources need sparse residency, which the layer does not offer (TiledResourcesTier is NOT_SUPPORTED).
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *, const SECURITY_ATTRIBUTES *, DWORD, LPCWSTR, HANDLE *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE *) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable *const *ppObjects) override;
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable *const *ppObjects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid, void **ppvCommandSignature) override;
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *, UINT *, D3D12_PACKED_MIP_INFO *, D3D12_TILE_SHAPE *, UINT *, UINT, D3D12_SUBRESOURCE_TILING *) override { D3D12M_STUB_LOG(); }
    D3D12M_AGGREGATE_RETURN(LUID, GetAdapterLuid, adapter_luid())
    // ID3D12Device1
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void *, SIZE_T, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence *const *, const UINT64 *, UINT, D3D12_MULTIPLE_FENCE_WAIT_FLAGS, HANDLE) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT, ID3D12Pageable *const *, const D3D12_RESIDENCY_PRIORITY *) override { return S_OK; }  // everything is resident
    // ID3D12Device2
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState) override;
    // ID3D12Device3
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS flags, UINT count, ID3D12Pageable *const *objects, ID3D12Fence *fence, UINT64 value) override;
    // ID3D12Device4
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *, REFIID riidResource, void **ppvResource) override
    {
        return CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC *pDesc, ID3D12ProtectedResourceSession *, REFIID riid, void **ppvHeap) override { return CreateHeap(pDesc, riid, ppvHeap); }
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *, REFIID riid, void **ppvResource) override
    {
        return CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    }
#ifdef _WIN32
    D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo1(D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT, UINT count, const D3D12_RESOURCE_DESC *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *info1) override { *ret = allocation_info(count, descs, info1); return ret; }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(UINT, UINT count, const D3D12_RESOURCE_DESC *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *info1) override { return allocation_info(count, descs, info1); }
#endif
    // ID3D12Device5
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner *, REFIID, void **) override { D3D12M_STUB_HR(); }
    void STDMETHODCALLTYPE RemoveDevice() override { D3D12M_STUB_LOG(); }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT *count, D3D12_META_COMMAND_DESC *) override
    {
        if (count)
            *count = 0;  // no meta commands
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(REFGUID, D3D12_META_COMMAND_PARAMETER_STAGE, UINT *, UINT *, D3D12_META_COMMAND_PARAMETER_DESC *) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID, UINT, const void *, SIZE_T, REFIID, void **) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC *, REFIID, void **) override { D3D12M_STUB_HR(); }
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) override
    {
        D3D12M_STUB_LOG();
        if (info)
            *info = {};
    }
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *) override
    {
        return D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    }
    // ID3D12Device6
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE, D3D12_MEASUREMENTS_ACTION, HANDLE, BOOL *further) override
    {
        if (further)
            *further = FALSE;
        return S_OK;
    }
    // ID3D12Device7
    HRESULT STDMETHODCALLTYPE AddToStateObject(const D3D12_STATE_OBJECT_DESC *, ID3D12StateObject *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *, REFIID, void **) override { D3D12M_STUB_HR(); }
    // ID3D12Device8
#ifdef _WIN32
    D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo2(D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT, UINT count, const D3D12_RESOURCE_DESC1 *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *info1) override { *ret = allocation_info1(count, descs, info1); return ret; }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(UINT, UINT count, const D3D12_RESOURCE_DESC1 *descs, D3D12_RESOURCE_ALLOCATION_INFO1 *info1) override { return allocation_info1(count, descs, info1); }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource2(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *, REFIID riidResource, void **ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource1(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource) override;
    void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(ID3D12Resource *, ID3D12Resource *, D3D12_CPU_DESCRIPTOR_HANDLE) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE GetCopyableFootprints1(const D3D12_RESOURCE_DESC1 *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes) override;
    // ID3D12Device9
    HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC *, REFIID, void **) override { D3D12M_STUB_HR(); }
    HRESULT STDMETHODCALLTYPE ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS, D3D12_SHADER_CACHE_CONTROL_FLAGS) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID, REFIID riid, void **ppCommandQueue) override { return CreateCommandQueue(pDesc, riid, ppCommandQueue); }
    // ID3D12Device10
    HRESULT STDMETHODCALLTYPE CreateCommittedResource3(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *, UINT32, D3D12M_CASTABLE_FORMATS, REFIID riidResource, void **ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource2(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, UINT32, D3D12M_CASTABLE_FORMATS, REFIID riid, void **ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource2(const D3D12_RESOURCE_DESC *, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *, UINT32, D3D12M_CASTABLE_FORMATS, REFIID, void **) override { D3D12M_STUB_HR(); }

private:
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info(UINT count, const D3D12_RESOURCE_DESC *descs,
                                                   D3D12_RESOURCE_ALLOCATION_INFO1 *info1 = nullptr) const;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info1(UINT count, const D3D12_RESOURCE_DESC1 *descs,
                                                    D3D12_RESOURCE_ALLOCATION_INFO1 *info1) const;
    HRESULT query_removed_extended_data(REFIID riid, void **out);
    LUID adapter_luid() const;

    Device() = default;
    ~Device() override;

    mtlb_device device_ = 0;
    std::mutex init_mutex_;
    std::vector<Resource *> pending_init_;
    bool resource_size_align(const D3D12_RESOURCE_DESC &desc, mtlb_size_align *out) const;
    std::shared_mutex heaps_mutex_;
    std::map<uintptr_t, DescriptorHeap *> heaps_;
    std::mutex null_mutex_;
    std::array<mtlb_descriptor, 16> null_descriptors_{};
    std::array<bool, 16> null_ready_{};
    mtlb_device_caps caps_{};
    FenceWaiter fence_waiter_;
};

} // namespace d3d12m
