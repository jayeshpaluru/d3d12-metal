// Geometry shader emulation: a shader that expands each point into a quad, drawn with DrawInstanced and
// DrawIndexedInstanced, and stream output (refused).
#include "geometry_gs.h"
#include "geometry_ps.h"
#include "geometry_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 64;
constexpr Pixel kBlack = {0, 0, 0, 255}, kRed = {255, 0, 0, 255};

Pixel pixel_at(const Image &image, float x, float y)
{
    return image.pixel(UINT((x + 1.0f) * 0.5f * kSize), UINT((1.0f - y) * 0.5f * kSize));
}

} // namespace

int main()
{
    Gpu gpu;
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature();
    ComPtr<ID3D12Resource> target = gpu.texture(
        tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_geometry_vs), T12_SHADER(g_geometry_ps), layout, 1);
    desc.GS = T12_SHADER(g_geometry_gs);
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(desc);

    // Stream output is refused, not silently ignored.
    {
        const D3D12_SO_DECLARATION_ENTRY entry = {0, "SV_Position", 0, 0, 4, 0};
        const UINT stride = 16;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC so = desc;
        so.StreamOutput = {&entry, 1, &stride, 1, 0};
        ComPtr<ID3D12PipelineState> refused;
        const HRESULT hr = gpu.device->CreateGraphicsPipelineState(&so, IID_PPV_ARGS(refused.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            std::fprintf(stderr, "a pipeline with stream output was created\n");
            return 1;
        }
    }

    const float points[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(points, sizeof(points));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(points), 8};
    const uint16_t indices[3] = {0, 1, 1};
    ComPtr<ID3D12Resource> index_buffer = gpu.upload_buffer(indices, sizeof(indices));
    const D3D12_INDEX_BUFFER_VIEW ibv = {index_buffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};

    auto render = [&](bool indexed, UINT count, UINT first) {
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            list->SetGraphicsRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clear[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            list->IASetVertexBuffers(0, 1, &vbv);
            if (indexed) {
                list->IASetIndexBuffer(&ibv);
                list->DrawIndexedInstanced(count, 1, first, 0, 0);
            } else {
                list->DrawInstanced(count, 1, first, 0);
            }
        });
        return gpu.read_texture(target.Get(), 0, 4);
    };

    {
        const Image image = render(false, 2, 0);
        expect_pixel("quad from point 0", pixel_at(image, -0.5f, 0.5f), kRed);
        expect_pixel("quad from point 1", pixel_at(image, 0.5f, -0.5f), kRed);
        expect_pixel("quad edge", pixel_at(image, -0.45f, 0.55f), kRed);
        expect_pixel("outside the quads", pixel_at(image, 0.0f, 0.0f), kBlack);
        expect_pixel("outside a quad", pixel_at(image, -0.3f, 0.5f), kBlack);
    }
    {
        const Image image = render(true, 1, 1);  // only the second point, through the index buffer
        expect_pixel("indexed: point 0 not drawn", pixel_at(image, -0.5f, 0.5f), kBlack);
        expect_pixel("indexed: quad from point 1", pixel_at(image, 0.5f, -0.5f), kRed);
    }
    {
        const Image image = render(false, 1, 1);  // first vertex 1
        expect_pixel("first vertex: point 0 not drawn", pixel_at(image, -0.5f, 0.5f), kBlack);
        expect_pixel("first vertex: quad from point 1", pixel_at(image, 0.5f, -0.5f), kRed);
    }
    std::printf("p_geometry passed\n");
    return 0;
}
