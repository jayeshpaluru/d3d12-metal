// A pipeline state created without a root signature takes the one embedded in its shader: the compute shader
// (tests/shaders/embedded_rs.hlsl) carries its signature, the pipeline is made with a null pRootSignature, the
// application builds an equal signature from the same bytecode and binds it.
#include "embedded_rs_cs.h"
#include "t12.h"

int main()
{
    Gpu gpu;
    ComPtr<ID3D12RootSignature> signature;
    CHECK_HR(gpu.device->CreateRootSignature(0, g_embedded_rs_cs, sizeof(g_embedded_rs_cs), IID_PPV_ARGS(signature.GetAddressOf())));

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = nullptr;
    desc.CS = T12_SHADER(g_embedded_rs_cs);
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(gpu.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf())));

    ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    gpu.run([&](ID3D12GraphicsCommandList *list) {
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        const UINT base = 1000;
        list->SetComputeRoot32BitConstants(0, 1, &base, 0);
        list->SetComputeRootUnorderedAccessView(1, buffer->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
    });
    const std::vector<uint8_t> data = gpu.read_buffer(buffer.Get(), 16 * 4);
    const uint32_t *values = reinterpret_cast<const uint32_t *>(data.data());
    for (UINT i = 0; i < 16; ++i) {
        if (values[i] != 1000 + i) {
            std::fprintf(stderr, "element %u is %u, expected %u\n", i, values[i], 1000 + i);
            return 1;
        }
    }

    // The same through a pipeline state stream without a root signature subobject.
    StreamObject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS> cs{T12_SHADER(g_embedded_rs_cs)};
    D3D12_PIPELINE_STATE_STREAM_DESC stream = {sizeof(cs), &cs};
    ComPtr<ID3D12Device2> device2;
    CHECK_HR(gpu.device->QueryInterface(IID_PPV_ARGS(device2.GetAddressOf())));
    ComPtr<ID3D12PipelineState> from_stream;
    CHECK_HR(device2->CreatePipelineState(&stream, IID_PPV_ARGS(from_stream.GetAddressOf())));

    // Shaders without an embedded signature still need one.
    ComPtr<ID3D12PipelineState> refused;
    D3D12_COMPUTE_PIPELINE_STATE_DESC bad = {};
    static const uint8_t junk[8] = {};
    bad.CS = {junk, sizeof(junk)};
    if (SUCCEEDED(gpu.device->CreateComputePipelineState(&bad, IID_PPV_ARGS(refused.GetAddressOf())))) {
        std::fprintf(stderr, "a pipeline without any root signature was created\n");
        return 1;
    }
    std::printf("p_embedded_rs passed\n");
    return 0;
}
