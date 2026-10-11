// A triangle with a vertex at an infinite position draws nothing, as D3D12 clips it (Metal rasterizes it: the layer
// turns infinite position components into NaN, which drops the primitive). A finite triangle beside it still draws.
#include <cmath>
#include <limits>

#include "inf_position_ps.h"
#include "inf_position_vs.h"
#include "t12.h"

int main()
{
    Gpu gpu;
    constexpr UINT kSize = 16;
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(nullptr, 0);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_inf_position_vs), T12_SHADER(g_inf_position_ps), layout, 1);
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(desc);

    const float inf = std::numeric_limits<float>::infinity();
    // Left half: a triangle with one vertex at (+inf, 0, 0, 0). Right half: an ordinary triangle.
    const float vertices[] = {-1, -1, 0.5f, 1,   -1, 1, 0.5f, 1,   inf, 0, 0, 0,
                              0.25f, -1, 0.5f, 1, 0.25f, 1, 0.5f, 1, 1, 0, 0.5f, 1};
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
    const D3D12_VERTEX_BUFFER_VIEW view = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 16};
    ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    gpu.run([&](ID3D12GraphicsCommandList *list) {
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &view);
        list->DrawInstanced(6, 1, 0, 0);
    });
    const Image image = gpu.read_texture(target.Get(), 0, 4);
    // Nothing of the infinite triangle anywhere left of the finite one (which starts at x = 0.25 in clip space).
    for (UINT y = 0; y < kSize; ++y)
        for (UINT x = 0; x < kSize * 5 / 8 - 1; ++x)
            expect_pixel("left of the finite triangle", image.pixel(x, y), {0, 0, 0, 255}, 0);
    expect_pixel("finite triangle", image.pixel(kSize * 3 / 4, kSize / 2), {255, 0, 0, 255}, 0);
    std::printf("p_inf_position: PASS\n");
    return 0;
}
