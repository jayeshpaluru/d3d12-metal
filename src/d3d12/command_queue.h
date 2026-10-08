// ID3D12CommandQueue backed by an mtlb queue.
#pragma once

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class CommandQueue final : public ChildImpl<ID3D12CommandQueue> {
public:
    static HRESULT create(Device *device, const D3D12_COMMAND_QUEUE_DESC &desc, REFIID riid, void **out);

    mtlb_queue handle() const { return queue_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(this, riid, out);
    }

    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *, UINT, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, ID3D12Heap *, UINT, const D3D12_TILE_RANGE_FLAGS *, const UINT *, const UINT *, D3D12_TILE_MAPPING_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, D3D12_TILE_MAPPING_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE EndEvent() override { D3D12M_STUB_LOG(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *, UINT64 *) override { D3D12M_STUB_HR(); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }

private:
    explicit CommandQueue(Device *device) : ChildImpl(device) {}
    ~CommandQueue() override;

    D3D12_COMMAND_QUEUE_DESC desc_{};
    mtlb_queue queue_ = 0;
};

} // namespace d3d12m
