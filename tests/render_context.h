// Offscreen rendering helpers for the colour-triangle tests: a render target,
// root signatures, a pipeline built from shaders/color.hlsl, and texture
// readback.
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
    Com<ID3D12Resource> target;
    Com<ID3D12DescriptorHeap> rtv_heap;

    RenderContext()
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kTargetSize;
        desc.Height = kTargetSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 IID_PPV_ARGS(target.put())));

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        CHECK_HR(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.put())));
        device->CreateRenderTargetView(target.get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv() { return rtv_heap->GetCPUDescriptorHandleForHeapStart(); }

    Com<ID3D12RootSignature> create_root_signature(const D3D12_ROOT_SIGNATURE_DESC1 &desc)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
        versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versioned.Desc_1_1 = desc;
        Com<ID3DBlob> blob, error;
        CHECK_HR(D3D12SerializeVersionedRootSignature(&versioned, blob.put(), error.put()));
        Com<ID3D12RootSignature> signature;
        CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(signature.put())));
        return signature;
    }

    // Pipeline drawing float3 POSITION vertices with the colour from shaders/color.hlsl.
    Com<ID3D12PipelineState> create_color_pso(ID3D12RootSignature *signature)
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = signature;
        desc.VS = {g_color_vs, sizeof(g_color_vs)};
        desc.PS = {g_color_ps, sizeof(g_color_ps)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.InputLayout = {layout, 1};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        Com<ID3D12PipelineState> pso;
        CHECK_HR(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.put())));
        return pso;
    }

    // Creates an upload buffer holding `data`.
    Com<ID3D12Resource> create_upload_buffer(const void *data, UINT64 size)
    {
        Com<ID3D12Resource> buffer = create_buffer(D3D12_HEAP_TYPE_UPLOAD, size);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, size);
        buffer->Unmap(0, nullptr);
        return buffer;
    }

    void set_viewport_and_scissor(ID3D12GraphicsCommandList *list)
    {
        D3D12_VIEWPORT viewport = {0, 0, float(kTargetSize), float(kTargetSize), 0, 1};
        D3D12_RECT scissor = {0, 0, LONG(kTargetSize), LONG(kTargetSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
    }

    // Records a copy of the render target into a readback buffer and returns it.
    Com<ID3D12Resource> record_readback(ID3D12GraphicsCommandList *list, UINT *row_pitch)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);

        D3D12_RESOURCE_DESC desc = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        *row_pitch = footprint.Footprint.RowPitch;
        Com<ID3D12Resource> readback = create_buffer(D3D12_HEAP_TYPE_READBACK, total);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
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
