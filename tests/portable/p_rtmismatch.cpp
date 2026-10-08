// A pixel shader output whose type differs from the render target format (an integer written to a normalised
// target, as games do with masked outputs) must not make the pipeline fail; the other targets render normally.
#include "rt_mismatch_ps.h"
#include "rt_mismatch_vs.h"
#include "t12.h"

int main()
{
    Gpu gpu;
    constexpr UINT kSize = 16;
    const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_rt_mismatch_vs), T12_SHADER(g_rt_mismatch_ps), layout, 1);
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;  // the shader writes uint4 here
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(desc);  // CHECKs that creation succeeds

    const float vertices[9] = {-1, -1, 0.5f, -1, 3, 0.5f, 3, -1, 0.5f};
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(vertices, sizeof(vertices));
    const D3D12_VERTEX_BUFFER_VIEW view = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 12};
    ComPtr<ID3D12Resource> targets[2];
    for (auto &target : targets)
        target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    const UINT increment = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    for (int i = 0; i < 2; ++i) {
        rtvs[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtvs[i].ptr += size_t(i) * increment;
        gpu.device->CreateRenderTargetView(targets[i].Get(), nullptr, rtvs[i]);
    }

    gpu.run([&](ID3D12GraphicsCommandList *list) {
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtvs[0], clear, 0, nullptr);
        list->ClearRenderTargetView(rtvs[1], clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &view);
        const float colour[4] = {0.25f, 0.5f, 1.0f, 1.0f};
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        list->DrawInstanced(3, 1, 0, 0);
    });
    const Image image = gpu.read_texture(targets[0].Get(), 0, 4);
    expect_pixel("target 0", image.pixel(kSize / 2, kSize / 2), {64, 128, 255, 255}, 2);
    return 0;
}
