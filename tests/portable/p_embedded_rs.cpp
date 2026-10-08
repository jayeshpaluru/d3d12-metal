// A pipeline state created without a root signature takes the one embedded in its shader: the compute shader
// (tests/shaders/embedded_rs.hlsl) carries its signature, the pipeline is made with a null pRootSignature, the
// application builds an equal signature from the same bytecode and binds it.
#include "embedded_rs_cs.h"
#include "embedded_rs_gfx_ps.h"
#include "embedded_rs_gfx_vs.h"
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

    // A graphics pipeline whose root signature is embedded in the pixel shader only. Two pipelines from the same
    // shaders (and their embedded signature) work, and so does drawing with a signature the application made.
    {
        ComPtr<ID3D12RootSignature> gfx_signature;
        CHECK_HR(gpu.device->CreateRootSignature(0, g_embedded_rs_gfx_ps, sizeof(g_embedded_rs_gfx_ps), IID_PPV_ARGS(gfx_signature.GetAddressOf())));
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx = graphics_pso_desc(nullptr, T12_SHADER(g_embedded_rs_gfx_vs), T12_SHADER(g_embedded_rs_gfx_ps), layout, 1);
        ComPtr<ID3D12PipelineState> first_gfx, second_gfx;
        CHECK_HR(gpu.device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(first_gfx.GetAddressOf())));
        gfx.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gfx.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        CHECK_HR(gpu.device->CreateGraphicsPipelineState(&gfx, IID_PPV_ARGS(second_gfx.GetAddressOf())));

        constexpr UINT kSize = 16;
        ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        const float triangle[] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
        ComPtr<ID3D12Resource> vertices = gpu.upload_buffer(triangle, sizeof(triangle));
        const D3D12_VERTEX_BUFFER_VIEW vbv = {vertices->GetGPUVirtualAddress(), sizeof(triangle), 12};
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(gfx_signature.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &vbv);
            const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
            list->SetPipelineState(first_gfx.Get());
            list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
            list->DrawInstanced(3, 1, 0, 0);
            list->SetPipelineState(second_gfx.Get());
            list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
            list->DrawInstanced(3, 1, 0, 0);
        });
        const Image image = gpu.read_texture(target.Get(), 0, 4);
        expect_pixel("pipeline with a pixel shader's embedded root signature", image.pixel(kSize / 2, kSize / 2), {0, 255, 0, 255});
    }
    std::printf("p_embedded_rs passed\n");
    return 0;
}
