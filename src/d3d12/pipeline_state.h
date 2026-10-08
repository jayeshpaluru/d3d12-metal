// ID3D12PipelineState for graphics pipelines.
#pragma once

#include "bridge/mtlb.h"
#include "d3d12/object.h"
#include "d3d12/root_signature.h"

namespace d3d12m {

class PipelineState final : public ChildImpl<ID3D12PipelineState> {
public:
    static HRESULT create_graphics(Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                                   REFIID riid, void **out);

    mtlb_pipeline handle() const { return pipeline_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12PipelineState>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **) override { D3D12M_STUB_HR(); }

private:
    explicit PipelineState(Device *device) : ChildImpl(device) {}
    ~PipelineState() override;

    mtlb_pipeline pipeline_ = 0;
    RootSignature *root_signature_ = nullptr;  // owned reference
};

} // namespace d3d12m
