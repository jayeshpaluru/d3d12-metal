// Hull and domain shader emulation: one triangle patch, tessellated by the fixed-function stage; the domain shader
// turns the barycentric coordinates into colours.
#include "t12.h"
#include "tessellation_ds.h"
#include "tessellation_hs.h"
#include "tessellation_ps.h"
#include "tessellation_vs.h"

namespace {

constexpr UINT kSize = 64;

UINT px(float x) { return UINT((x + 1.0f) * 0.5f * kSize); }
UINT py(float y) { return UINT((1.0f - y) * 0.5f * kSize); }

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
        graphics_pso_desc(signature.Get(), T12_SHADER(g_tessellation_vs), T12_SHADER(g_tessellation_ps), layout, 1);
    desc.HS = T12_SHADER(g_tessellation_hs);
    desc.DS = T12_SHADER(g_tessellation_ds);
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(desc);

    // One patch: bary.x belongs to the first control point (bottom left), y to the second (top), z to the third.
    const float patch[6] = {-0.8f, -0.8f, 0.0f, 0.8f, 0.8f, -0.8f};
    ComPtr<ID3D12Resource> vertex_buffer = gpu.upload_buffer(patch, sizeof(patch));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(patch), 8};

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
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->DrawInstanced(3, 1, 0, 0);
    });
    const Image image = gpu.read_texture(target.Get(), 0, 4);

    // Close to each corner the colour is that corner's primary; at the centroid the three weights are equal.
    const Pixel near_first = image.pixel(px(-0.7f), py(-0.72f));
    const Pixel near_second = image.pixel(px(0.0f), py(0.7f));
    const Pixel near_third = image.pixel(px(0.7f), py(-0.72f));
    const Pixel centre = image.pixel(px(0.0f), py(-0.267f));
    const Pixel outside = image.pixel(px(-0.75f), py(0.7f));
    expect_pixel("outside the patch", outside, {0, 0, 0, 255});
    if (!(near_first.r > 200 && near_first.g < 60 && near_first.b < 60)
        || !(near_second.g > 200 && near_second.r < 60 && near_second.b < 60)
        || !(near_third.b > 200 && near_third.r < 60 && near_third.g < 60)) {
        std::fprintf(stderr, "corner colours (%u,%u,%u) (%u,%u,%u) (%u,%u,%u)\n", near_first.r, near_first.g, near_first.b, near_second.r,
                     near_second.g, near_second.b, near_third.r, near_third.g, near_third.b);
        return 1;
    }
    expect_pixel("centroid", centre, {85, 85, 85, 255}, 40);
    std::printf("p_tessellation passed\n");
    return 0;
}
