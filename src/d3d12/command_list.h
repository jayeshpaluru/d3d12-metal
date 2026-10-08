// ID3D12GraphicsCommandList (implemented up to ID3D12GraphicsCommandList1).
//
// Calls are recorded into an mtlb command stream (bridge/mtlb_cmd.h) that
// ExecuteCommandLists hands to the backend. The records mirror the D3D12 calls;
// the backend decides how they map onto Metal render passes.
#pragma once

#include <vector>

#include "bridge/mtlb_cmd.h"
#include "common/log.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/object.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

class CommandList final : public ChildImpl<ID3D12GraphicsCommandList7> {
public:
    // Creates a list in the recording state.
    static HRESULT create(Device *device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
                          ID3D12PipelineState *initial_state, REFIID riid, void **out);

    bool closed() const { return closed_; }
    const std::vector<uint8_t> &stream() const { return stream_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList,
                                ID3D12GraphicsCommandList, ID3D12GraphicsCommandList1, ID3D12GraphicsCommandList2,
                                ID3D12GraphicsCommandList3, ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList5,
                                ID3D12GraphicsCommandList6, ID3D12GraphicsCommandList7>(this, riid, out);
    }

    // ID3D12CommandList
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }

    // ID3D12GraphicsCommandList
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, ID3D12Resource *, UINT64, D3D12_TILE_COPY_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *, UINT, ID3D12Resource *, UINT, DXGI_FORMAT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[ 4 ]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pPipelineState) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource *, const UINT[ 4 ], UINT, const D3D12_RECT *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource *, const FLOAT[ 4 ], UINT, const D3D12_RECT *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *, const D3D12_DISCARD_REGION *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource *, UINT64) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *, UINT64, D3D12_PREDICATION_OP) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE EndEvent() override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList1
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT, ID3D12Resource *const *, const D3D12_SUBRESOURCE_RANGE_UINT64 *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT, ID3D12Resource *const *, const D3D12_SUBRESOURCE_RANGE_UINT64 *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT, FLOAT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetSamplePositions(UINT, UINT, D3D12_SAMPLE_POSITION *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource *, UINT, UINT, UINT, ID3D12Resource *, UINT, D3D12_RECT *, DXGI_FORMAT, D3D12_RESOLVE_MODE) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList2
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *, const D3D12_WRITEBUFFERIMMEDIATE_MODE *) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList3
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession *) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList4
    void STDMETHODCALLTYPE BeginRenderPass(UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *, D3D12_RENDER_PASS_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE EndRenderPass() override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand *, const void *, SIZE_T) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *, UINT, const D3D12_GPU_VIRTUAL_ADDRESS *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList5
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER *) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource *) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList6
    void STDMETHODCALLTYPE DispatchMesh(UINT, UINT, UINT) override { D3D12M_STUB_LOG(); }
    // ID3D12GraphicsCommandList7
    void STDMETHODCALLTYPE Barrier(UINT32, const D3D12_BARRIER_GROUP *) override { D3D12M_STUB_LOG(); }

private:
    CommandList(Device *device, D3D12_COMMAND_LIST_TYPE type) : ChildImpl(device), type_(type) {}
    ~CommandList() override;

    // The root signature and root arguments of one pipeline type. The arguments are the shader
    // converter's top-level argument buffer, kept as bytes and sent whole before the next draw or dispatch.
    struct RootState {
        RootSignature *signature = nullptr;  // owned reference
        std::vector<uint8_t> args;
        bool dirty = false;
    };

    void reset_state();
    template <typename T>
    T *append(mtlb_cmd_type type, size_t extra_bytes = 0);
    bool prepare_draw();
    void set_root_signature(RootState &state, ID3D12RootSignature *signature, const char *what);
    const RootSignature::Slot *find_slot(RootState &state, UINT index, D3D12_ROOT_PARAMETER_TYPE type);
    void set_root_address(RootState &state, UINT index, D3D12_ROOT_PARAMETER_TYPE type, uint64_t address);
    void set_root_constants(RootState &state, UINT index, UINT count, const void *data, UINT dest_offset);
    void flush_root_args(RootState &state, mtlb_cmd_type type);
    void copy_texture_to_texture(const D3D12_TEXTURE_COPY_LOCATION &dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                 const D3D12_TEXTURE_COPY_LOCATION &src, const D3D12_BOX *src_box);

    D3D12_COMMAND_LIST_TYPE type_;
    bool closed_ = false;
    std::vector<uint8_t> stream_;

    bool has_graphics_pipeline_ = false;
    bool has_compute_pipeline_ = false;
    RootState graphics_;
    RootState compute_;
};

} // namespace d3d12m
