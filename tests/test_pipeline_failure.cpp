// A pipeline whose creation fails is not created again for the same description (games retry every frame).
#include <cstring>

#include "bridge/mtlb.h"
#include "d3d12/device.h"
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
    auto creations = [] {
        mtlb_stats stats;
        mtlb_stats_get(&stats);
        return stats.pipeline_attempts;
    };

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

    // The cache is keyed by what the root signature describes, not by the object: a second, equal signature finds the
    // failure of the first (and only once; no further attempts).
    ComPtr<ID3D12RootSignature> twin;
    CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(twin.GetAddressOf())));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g2 = g;
    g2.pRootSignature = twin.Get();
    g2.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    const uint64_t known = creations();
    CHECK(device->CreateGraphicsPipelineState(&g2, IID_PPV_ARGS(pso.GetAddressOf())) == first);
    CHECK(creations() == known);
    // A different signature is another description.
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC other_rs = {};
    other_rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    other_rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> other_blob;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&other_rs, other_blob.GetAddressOf(), error.ReleaseAndGetAddressOf()));
    ComPtr<ID3D12RootSignature> other;
    CHECK_HR(device->CreateRootSignature(0, other_blob->GetBufferPointer(), other_blob->GetBufferSize(), IID_PPV_ARGS(other.GetAddressOf())));
    g2.pRootSignature = other.Get();
    CHECK(FAILED(device->CreateGraphicsPipelineState(&g2, IID_PPV_ARGS(pso.GetAddressOf()))));
    CHECK(creations() == known + 1);

    // A failure that comes from the moment (memory) is not remembered: the next attempt runs again.
    D3D12_COMPUTE_PIPELINE_STATE_DESC transient = c;
    transient.CS = {junk, sizeof(junk) - 1};
    mtlb_device_test_fail_next_pipeline(static_cast<d3d12m::Device *>(device)->handle(), MTLB_ERROR_OUT_OF_MEMORY);
    const uint64_t attempts = creations();
    CHECK(device->CreateComputePipelineState(&transient, IID_PPV_ARGS(pso.GetAddressOf())) == E_OUTOFMEMORY);
    const HRESULT again = device->CreateComputePipelineState(&transient, IID_PPV_ARGS(pso.GetAddressOf()));
    CHECK(again != E_OUTOFMEMORY && FAILED(again));  // the junk shader itself fails now, with its own error
    CHECK(creations() == attempts + 2);
    return 0;
}
