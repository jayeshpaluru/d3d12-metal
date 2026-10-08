// A pipeline whose creation fails is not created again for the same description (games retry every frame).
#include <cstring>

#include "common/stats.h"
#include "test_context.h"

int main()
{
    TestContext ctx;
    ID3D12Device *device = ctx.device.Get();

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&rs, blob.GetAddressOf(), error.GetAddressOf()));
    ComPtr<ID3D12RootSignature> signature;
    CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature.GetAddressOf())));

    const char junk[64] = "not a shader";
    d3d12m::g_stats_enabled = true;
    auto creations = [] { return d3d12m::g_stat_values[static_cast<unsigned>(d3d12m::Stat::PsoCreations)].load(); };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = signature.Get();
    g.VS = {junk, sizeof(junk)};
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.SampleDesc.Count = 1;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12PipelineState> pso;
    const uint64_t before = creations();
    const HRESULT first = device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf()));
    CHECK(FAILED(first));
    CHECK(creations() == before + 1);
    for (int i = 0; i < 5; ++i)
        CHECK(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf())) == first);
    CHECK(creations() == before + 1);

    // Another description is tried on its own.
    g.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    CHECK(FAILED(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(pso.GetAddressOf()))));
    CHECK(creations() == before + 2);

    D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
    c.pRootSignature = signature.Get();
    c.CS = {junk, sizeof(junk)};
    const HRESULT compute = device->CreateComputePipelineState(&c, IID_PPV_ARGS(pso.GetAddressOf()));
    CHECK(FAILED(compute));
    CHECK(device->CreateComputePipelineState(&c, IID_PPV_ARGS(pso.GetAddressOf())) == compute);
    CHECK(creations() == before + 3);
    return 0;
}
