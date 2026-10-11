// SPDX-License-Identifier: LGPL-2.1-or-later
// Shared pieces of the Win32 test programs (built with MinGW, run under Wine).
// They use the MinGW D3D12/DXGI headers, so they exercise the layer through the
// same binary interface a Windows game uses.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stderr);                                                        \
            std::exit(1);                                                               \
        }                                                                               \
    } while (0)

#define CHECK_HR(expr)                                                                          \
    do {                                                                                        \
        HRESULT hr_ = (expr);                                                                   \
        if (FAILED(hr_)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s failed: 0x%08x\n", __FILE__, __LINE__, #expr,       \
                         static_cast<unsigned>(hr_));                                           \
            std::fflush(stderr);                                                                \
            std::exit(1);                                                                       \
        }                                                                                       \
    } while (0)

inline D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

inline D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

inline D3D12_RESOURCE_DESC texture_desc(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

struct Pixel {
    unsigned char r, g, b, a;
};

inline bool near_pixel(Pixel a, Pixel b, int tolerance = 2)
{
    auto close = [&](int x, int y) { return (x > y ? x - y : y - x) <= tolerance; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b) && close(a.a, b.a);
}

// Creates a device on the first adapter and a direct queue.
struct DeviceContext {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;

    DeviceContext()
    {
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        CHECK_HR(factory->EnumAdapters1(0, &adapter));
        CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }

    // Blocks until everything submitted so far has finished.
    void wait_idle()
    {
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(event);
        CHECK_HR(fence->SetEventOnCompletion(fence_value, event));
        CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        CloseHandle(event);
    }

    ComPtr<ID3D12Resource> create_buffer(D3D12_HEAP_TYPE type, UINT64 size)
    {
        const D3D12_HEAP_PROPERTIES heap = heap_properties(type);
        const D3D12_RESOURCE_DESC desc = buffer_desc(size);
        ComPtr<ID3D12Resource> buffer;
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&buffer)));
        return buffer;
    }

    ComPtr<ID3D12Resource> create_upload_buffer(const void *data, UINT64 size)
    {
        ComPtr<ID3D12Resource> buffer = create_buffer(D3D12_HEAP_TYPE_UPLOAD, size);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, size);
        buffer->Unmap(0, nullptr);
        return buffer;
    }

    ComPtr<ID3D12RootSignature> create_root_signature(const D3D12_ROOT_PARAMETER *parameters, UINT count)
    {
        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = count;
        desc.pParameters = parameters;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error;
        CHECK_HR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
        ComPtr<ID3D12RootSignature> signature;
        CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&signature)));
        return signature;
    }
};

// A pipeline state description with the defaults the tests use: no culling, no
// blending, no depth, triangle list, one render target of `format`.
inline D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pipeline_desc(ID3D12RootSignature *signature,
                                                                const D3D12_INPUT_ELEMENT_DESC *layout,
                                                                UINT num_elements, const void *vs, SIZE_T vs_size,
                                                                const void *ps, SIZE_T ps_size, DXGI_FORMAT format)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = signature;
    desc.VS = {vs, vs_size};
    desc.PS = {ps, ps_size};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.InputLayout = {layout, num_elements};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    return desc;
}
