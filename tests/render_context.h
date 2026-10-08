// Offscreen rendering helpers for the colour-triangle tests: a render target,
// root signatures, pipelines, and texture readback.
#pragma once

#include <cstring>
#include <vector>

#include "color_ps.h"
#include "color_vs.h"
#include "test_context.h"

constexpr UINT kTargetSize = 64;

struct Pixel {
    uint8_t r, g, b, a;
};

struct RenderContext : TestContext {
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;

    RenderContext()
    {
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, kTargetSize, kTargetSize, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(target.ReleaseAndGetAddressOf())));

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        CHECK_HR(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.ReleaseAndGetAddressOf())));
        device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv() { return rtv_heap->GetCPUDescriptorHandleForHeapStart(); }

    // A root signature with the given parameters that allows an input layout.
    ComPtr<ID3D12RootSignature> create_root_signature(const D3D12_ROOT_PARAMETER1 *parameters = nullptr,
                                                      UINT num_parameters = 0)
    {
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(num_parameters, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> blob, error;
        CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, blob.ReleaseAndGetAddressOf(), error.ReleaseAndGetAddressOf()));
        ComPtr<ID3D12RootSignature> signature;
        CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(signature.ReleaseAndGetAddressOf())));
        return signature;
    }

    // A triangle-list pipeline drawing to the render target, no depth, no culling.
    ComPtr<ID3D12PipelineState> create_pso(ID3D12RootSignature *signature, D3D12_SHADER_BYTECODE vs,
                                           D3D12_SHADER_BYTECODE ps, const D3D12_INPUT_ELEMENT_DESC *layout,
                                           UINT num_elements)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = signature;
        desc.VS = vs;
        desc.PS = ps;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.InputLayout = {layout, num_elements};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        CHECK_HR(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf())));
        return pso;
    }

    // Pipeline drawing float3 POSITION vertices with the colour from shaders/color.hlsl.
    ComPtr<ID3D12PipelineState> create_color_pso(ID3D12RootSignature *signature)
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        return create_pso(signature, {g_color_vs, sizeof(g_color_vs)}, {g_color_ps, sizeof(g_color_ps)}, layout, 1);
    }

    void set_viewport_and_scissor(ID3D12GraphicsCommandList *list)
    {
        D3D12_VIEWPORT viewport = {0, 0, float(kTargetSize), float(kTargetSize), 0, 1};
        D3D12_RECT scissor = {0, 0, LONG(kTargetSize), LONG(kTargetSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
    }

    // Records a copy of the render target into a readback buffer and returns it.
    ComPtr<ID3D12Resource> record_readback(ID3D12GraphicsCommandList *list, UINT *row_pitch)
    {
        const CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);

        D3D12_RESOURCE_DESC desc = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        *row_pitch = footprint.Footprint.RowPitch;
        ComPtr<ID3D12Resource> readback = create_buffer(D3D12_HEAP_TYPE_READBACK, total);

        const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION src(target.Get(), 0);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return readback;
    }
};

// Reads pixel (x, y) from a readback buffer filled by record_readback.
inline Pixel read_pixel(ID3D12Resource *readback, UINT row_pitch, UINT x, UINT y)
{
    void *mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    Pixel pixel;
    std::memcpy(&pixel, static_cast<uint8_t *>(mapped) + size_t(y) * row_pitch + x * sizeof(Pixel), sizeof(Pixel));
    readback->Unmap(0, nullptr);
    return pixel;
}

inline bool near_color(Pixel p, Pixel expected)
{
    auto close = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) <= 2; };
    return close(p.r, expected.r) && close(p.g, expected.g) && close(p.b, expected.b) && close(p.a, expected.a);
}

inline void check_pixel(const char *what, Pixel actual, Pixel expected)
{
    if (!near_color(actual, expected)) {
        std::fprintf(stderr, "%s: got (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n", what, actual.r, actual.g, actual.b,
                     actual.a, expected.r, expected.g, expected.b, expected.a);
        std::exit(1);
    }
}
