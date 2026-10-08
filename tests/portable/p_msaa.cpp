// Multisampling: 4x render targets and depth buffers, sample count queries, ResolveSubresource, and reading
// the samples of a multisampled texture in a shader.
#include <cmath>

#include "color_ps.h"
#include "color_vs.h"
#include "msaa_read_cs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 32;
constexpr UINT kSamples = 4;

struct Float4 {
    float x, y, z, w;
};

} // namespace

int main()
{
    Gpu gpu;

    // ---- Sample count support ---------------------------------------------------------------------------------------
    for (UINT count : {1u, 2u, 4u, 8u}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {DXGI_FORMAT_R8G8B8A8_UNORM, count, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        CHECK_HR(gpu.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)));
        CHECK(levels.NumQualityLevels >= 1 || count == 8);  // Apple GPUs do 1, 2 and 4; 8 depends on the chip
    }
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth_levels = {DXGI_FORMAT_D32_FLOAT, kSamples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    CHECK_HR(gpu.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth_levels, sizeof(depth_levels)));
    CHECK(depth_levels.NumQualityLevels >= 1);

    // ---- A multisampled render target and depth buffer, resolved ---------------------------------------------------------
    ComPtr<ID3D12Resource> msaa = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, 1, kSamples),
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12Resource> resolved = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                  D3D12_RESOURCE_STATE_RESOLVE_DEST);
    ComPtr<ID3D12Resource> depth = gpu.texture(tex2d_desc(DXGI_FORMAT_D32_FLOAT, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, 1, 1, kSamples),
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    ComPtr<ID3D12DescriptorHeap> dsv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    gpu.device->CreateRenderTargetView(msaa.Get(), &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    gpu.device->CreateDepthStencilView(depth.Get(), &dsv_desc, dsv_heap->GetCPUDescriptorHandleForHeapStart());

    const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(&constants, 1);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc =
        graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1);
    pso_desc.SampleDesc.Count = kSamples;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(pso_desc);

    // A triangle with slanted edges: pixels on them are partly covered. A second, nearer one is behind it in draw
    // order and in front in depth, so the depth buffer decides per sample.
    const float triangle[] = {-0.9f, -0.8f, 0.5f, 0.9f, -0.3f, 0.5f, -0.2f, 0.9f, 0.5f};
    ComPtr<ID3D12Resource> vertices = gpu.upload_buffer(triangle, sizeof(triangle));
    const D3D12_VERTEX_BUFFER_VIEW vbv = {vertices->GetGPUVirtualAddress(), sizeof(triangle), 12};

    gpu.run([&](ID3D12GraphicsCommandList *list) {
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const float red[4] = {1, 0, 0, 1};
        list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(msaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE)};
        list->ResourceBarrier(1, barriers);
        list->ResolveSubresource(resolved.Get(), 0, msaa.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    });

    const Image image = gpu.read_texture(resolved.Get(), 0, 4);
    int interior = 0, outside = 0, edge = 0;
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            const Pixel p = image.pixel(x, y);
            if (p.r == 255 && p.g == 0 && p.b == 0) {
                ++interior;
            } else if (p.r == 0 && p.g == 0 && p.b == 255) {
                ++outside;
            } else {
                ++edge;
                // A partly covered pixel is a mix of red and blue by k/4, k = 1..3 samples.
                bool mix = false;
                for (int k = 1; k < static_cast<int>(kSamples); ++k) {
                    const float f = float(k) / kSamples;
                    mix = mix || (std::fabs(p.r - 255 * f) <= 3 && std::fabs(p.b - 255 * (1 - f)) <= 3 && p.g == 0);
                }
                if (!mix) {
                    std::fprintf(stderr, "pixel (%u,%u) = (%u,%u,%u) is not a coverage mix\n", x, y, p.r, p.g, p.b);
                    return 1;
                }
            }
        }
    }
    std::printf("p_msaa: %d inside, %d outside, %d edge pixels\n", interior, outside, edge);
    CHECK(interior > 100 && outside > 100 && edge > 10);

    // ---- The samples of an edge pixel, read by a shader ------------------------------------------------------------------
    {
        UINT ex = 0, ey = 0;
        for (UINT y = 0; y < kSize && !ex; ++y) {
            for (UINT x = 0; x < kSize; ++x) {
                const Pixel p = image.pixel(x, y);
                if (p.r != 255 && p.r != 0) {
                    ex = x;
                    ey = y;
                    break;
                }
            }
        }
        const Pixel mixed = image.pixel(ex, ey);
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1),
                                                    root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
        ComPtr<ID3D12RootSignature> read_signature = gpu.root_signature(parameters, 3, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3D12PipelineState> read_pso = gpu.compute_pso(read_signature.Get(), T12_SHADER(g_msaa_read_cs));
        ComPtr<ID3D12DescriptorHeap> srv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        gpu.device->CreateShaderResourceView(msaa.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
        ComPtr<ID3D12Resource> results = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSamples * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(read_signature.Get());
            list->SetPipelineState(read_pso.Get());
            const UINT params[4] = {ex, ey, kSamples, 0};
            list->SetComputeRoot32BitConstants(0, 4, params, 0);
            list->SetComputeRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
        });
        const std::vector<uint8_t> bytes = gpu.read_buffer(results.Get(), kSamples * 16);
        Float4 samples[kSamples];
        std::memcpy(samples, bytes.data(), sizeof(samples));
        int red_samples = 0;
        for (const Float4 &s : samples) {
            const bool is_red = s.x > 0.99f && s.z < 0.01f, is_blue = s.x < 0.01f && s.z > 0.99f;
            CHECK(is_red || is_blue);  // every sample is one colour or the other
            red_samples += is_red;
        }
        // The resolved pixel is the average of its samples.
        CHECK(red_samples >= 1 && red_samples <= 3);
        CHECK(std::fabs(mixed.r - 255.0f * red_samples / kSamples) <= 3);
    }

    // ---- Resolve with a format, and a typeless multisampled resource ------------------------------------------------------------
    {
        ComPtr<ID3D12Resource> typeless = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 8, 8, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 1, 1, kSamples),
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_TYPELESS, 8, 8, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                    D3D12_RESOURCE_STATE_RESOLVE_DEST);
        ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        D3D12_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        gpu.device->CreateRenderTargetView(typeless.Get(), &desc, heap->GetCPUDescriptorHandleForHeapStart());
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(), colour, 0, nullptr);
            list->ResolveSubresource(target.Get(), 0, typeless.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        });
        const Image resolved_image = gpu.read_texture(target.Get(), 0, 4);
        expect_pixel("resolved clear colour", resolved_image.pixel(3, 3), {64, 128, 191, 255}, 1);
    }

    std::printf("p_msaa: PASS\n");
    return 0;
}
