// Helpers for the portable tests: the same test sources build natively (arm64
// macOS, DirectX-Headers, run against the layer's dylib) and as Win32 programs
// (MinGW headers, run under Wine against d3d12.dll). Everything here compiles
// with both header sets, so it avoids d3dx12.h, which needs newer Windows
// headers than MinGW ships.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#else
#include "common/d3d12_uuids.h"
#include "dxgi/dxgi_interfaces.h"

#include <wsl/wrladapter.h>

extern "C" {
HRESULT D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice);
HRESULT D3D12GetDebugInterface(REFIID riid, void **ppvDebug);
}
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stderr);                                                          \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_HR(expr)                                                                           \
    do {                                                                                         \
        HRESULT hr_ = (expr);                                                                    \
        if (FAILED(hr_)) {                                                                       \
            std::fprintf(stderr, "%s:%d: %s failed: 0x%08x\n", __FILE__, __LINE__, #expr,        \
                         static_cast<unsigned>(hr_));                                            \
            std::fflush(stderr);                                                                 \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

// Checks `value` against `expected` and reports both on failure.
#define CHECK_EQ(value, expected)                                                                      \
    do {                                                                                               \
        const auto v_ = (value);                                                                       \
        const auto e_ = (expected);                                                                    \
        if (!(v_ == e_)) {                                                                             \
            std::fprintf(stderr, "%s:%d: %s == %s failed: got %lld, expected %lld\n", __FILE__, __LINE__, \
                         #value, #expected, static_cast<long long>(v_), static_cast<long long>(e_));   \
            std::fflush(stderr);                                                                       \
            std::exit(1);                                                                              \
        }                                                                                              \
    } while (0)

namespace t12 {

// ---- Descriptions -----------------------------------------------------------------------------

inline D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

inline D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_DESC texture_desc(D3D12_RESOURCE_DIMENSION dimension, DXGI_FORMAT format, UINT64 width,
                                        UINT height, UINT depth_or_array, UINT mips, D3D12_RESOURCE_FLAGS flags,
                                        UINT samples = 1)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = dimension;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth_or_array);
    desc.MipLevels = static_cast<UINT16>(mips);
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Flags = flags;
    return desc;
}

inline D3D12_RESOURCE_DESC tex2d_desc(DXGI_FORMAT format, UINT64 width, UINT height,
                                      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, UINT array_size = 1,
                                      UINT mips = 1, UINT samples = 1)
{
    return texture_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, format, width, height, array_size, mips, flags, samples);
}

inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after,
                                         UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = subresource;
    return barrier;
}

inline D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource *resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

inline D3D12_RESOURCE_BARRIER aliasing_barrier(ID3D12Resource *before, ID3D12Resource *after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = before;
    barrier.Aliasing.pResourceAfter = after;
    return barrier;
}

inline D3D12_TEXTURE_COPY_LOCATION subresource_location(ID3D12Resource *resource, UINT subresource)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = subresource;
    return location;
}

inline D3D12_TEXTURE_COPY_LOCATION footprint_location(ID3D12Resource *buffer,
                                                      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint)
{
    D3D12_TEXTURE_COPY_LOCATION location = {};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint = footprint;
    return location;
}

inline D3D12_SHADER_BYTECODE bytecode(const void *data, size_t size)
{
    return {data, size};
}

#define T12_SHADER(name) t12::bytecode(name, sizeof(name))

inline D3D12_ROOT_PARAMETER1 root_constants(UINT register_index, UINT count,
                                            D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants = {register_index, 0, count};
    p.ShaderVisibility = visibility;
    return p;
}

inline D3D12_ROOT_PARAMETER1 root_descriptor(D3D12_ROOT_PARAMETER_TYPE type, UINT register_index,
                                             D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = type;
    p.Descriptor = {register_index, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    p.ShaderVisibility = visibility;
    return p;
}

inline D3D12_DESCRIPTOR_RANGE1 descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE type, UINT count, UINT base_register,
                                                UINT space = 0)
{
    D3D12_DESCRIPTOR_RANGE1 range = {};
    range.RangeType = type;
    range.NumDescriptors = count;
    range.BaseShaderRegister = base_register;
    range.RegisterSpace = space;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    return range;
}

// `ranges` must outlive the root signature description.
inline D3D12_ROOT_PARAMETER1 descriptor_table(const D3D12_DESCRIPTOR_RANGE1 *ranges, UINT count,
                                              D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable = {count, ranges};
    p.ShaderVisibility = visibility;
    return p;
}

inline D3D12_STATIC_SAMPLER_DESC static_sampler(UINT shader_register, D3D12_FILTER filter,
                                                D3D12_TEXTURE_ADDRESS_MODE address, UINT space = 0,
                                                D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL)
{
    D3D12_STATIC_SAMPLER_DESC s = {};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = address;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MinLOD = 0;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = shader_register;
    s.RegisterSpace = space;
    s.ShaderVisibility = visibility;
    return s;
}

inline D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_pso_desc(ID3D12RootSignature *signature, D3D12_SHADER_BYTECODE vs,
                                                            D3D12_SHADER_BYTECODE ps,
                                                            const D3D12_INPUT_ELEMENT_DESC *layout, UINT num_elements,
                                                            DXGI_FORMAT rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = signature;
    desc.VS = vs;
    desc.PS = ps;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    for (auto &target : desc.BlendState.RenderTarget) {
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
    }
    desc.SampleMask = UINT_MAX;
    desc.InputLayout = {layout, num_elements};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = rtv_format == DXGI_FORMAT_UNKNOWN ? 0 : 1;
    desc.RTVFormats[0] = rtv_format;
    desc.SampleDesc.Count = 1;
    return desc;
}

// A subobject of a pipeline state stream: pointer-aligned, the type, then the payload.
template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void *) StreamObject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
    StreamObject() = default;
    StreamObject(const T &v) : value(v) {}
};

// ---- Pixels ---------------------------------------------------------------------------------------

struct Pixel {
    uint8_t r, g, b, a;
};

inline bool near_pixel(Pixel a, Pixel b, int tolerance = 2)
{
    auto close = [&](int x, int y) { return (x > y ? x - y : y - x) <= tolerance; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b) && close(a.a, b.a);
}

inline void expect_pixel(const char *what, Pixel actual, Pixel expected, int tolerance = 2)
{
    if (!near_pixel(actual, expected, tolerance)) {
        std::fprintf(stderr, "%s: got (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n", what, actual.r, actual.g, actual.b,
                     actual.a, expected.r, expected.g, expected.b, expected.a);
        std::fflush(stderr);
        std::exit(1);
    }
}

// ---- The device and queue ----------------------------------------------------------------------------

// A mapped texture read back to the CPU, rows tightly packed per slice.
struct Image {
    UINT width = 0, height = 0, depth = 1;
    UINT bytes_per_pixel = 0;
    std::vector<uint8_t> data;

    const uint8_t *at(UINT x, UINT y, UINT z = 0) const
    {
        return data.data() + ((size_t(z) * height + y) * width + x) * bytes_per_pixel;
    }
    Pixel pixel(UINT x, UINT y, UINT z = 0) const
    {
        Pixel p;
        std::memcpy(&p, at(x, y, z), sizeof(p));
        return p;
    }
};

struct Gpu {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;  // one per list made by list()

    Gpu()
    {
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())));
        ComPtr<IDXGIAdapter1> adapter;
        CHECK_HR(factory->EnumAdapters1(0, adapter.GetAddressOf()));
        CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf())));
        CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    }

    // Blocks until everything submitted to the main queue has finished.
    void wait_idle()
    {
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        CHECK_HR(fence->SetEventOnCompletion(fence_value, nullptr));
        CHECK(fence->GetCompletedValue() >= fence_value);
    }

    ComPtr<ID3D12Resource> committed(D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC &desc,
                                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
                                     const D3D12_CLEAR_VALUE *clear = nullptr)
    {
        const D3D12_HEAP_PROPERTIES heap = heap_props(type);
        ComPtr<ID3D12Resource> resource;
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
                                                 IID_PPV_ARGS(resource.GetAddressOf())));
        return resource;
    }

    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        return committed(type, buffer_desc(size, flags));
    }

    ComPtr<ID3D12Resource> upload_buffer(const void *data, UINT64 size)
    {
        ComPtr<ID3D12Resource> buffer = this->buffer(D3D12_HEAP_TYPE_UPLOAD, size);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, size);
        buffer->Unmap(0, nullptr);
        return buffer;
    }

    ComPtr<ID3D12Resource> texture(const D3D12_RESOURCE_DESC &desc,
                                   D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
                                   const D3D12_CLEAR_VALUE *clear = nullptr)
    {
        return committed(D3D12_HEAP_TYPE_DEFAULT, desc, state, clear);
    }

    ComPtr<ID3D12DescriptorHeap> descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shader_visible = false)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> heap;
        CHECK_HR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
        return heap;
    }

    UINT increment(D3D12_DESCRIPTOR_HEAP_TYPE type) { return device->GetDescriptorHandleIncrementSize(type); }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *heap, UINT index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(index) * increment(heap->GetDesc().Type);
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *heap, UINT index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(index) * increment(heap->GetDesc().Type);
        return handle;
    }

    ComPtr<ID3D12GraphicsCommandList> list(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           ID3D12PipelineState *initial_state = nullptr)
    {
        allocators.emplace_back();
        CHECK_HR(device->CreateCommandAllocator(type, IID_PPV_ARGS(allocators.back().GetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> list;
        CHECK_HR(device->CreateCommandList(0, type, allocators.back().Get(), initial_state,
                                           IID_PPV_ARGS(list.GetAddressOf())));
        return list;
    }

    void execute(ID3D12GraphicsCommandList *list, ID3D12CommandQueue *on = nullptr)
    {
        ID3D12CommandList *lists[] = {list};
        (on ? on : queue.Get())->ExecuteCommandLists(1, lists);
    }

    // Closes `list`, executes it on the main queue and waits.
    void run(ID3D12GraphicsCommandList *list)
    {
        CHECK_HR(list->Close());
        execute(list);
        wait_idle();
    }

    // Records `body` into a fresh list and runs it.
    void run(const std::function<void(ID3D12GraphicsCommandList *)> &body)
    {
        ComPtr<ID3D12GraphicsCommandList> l = list();
        body(l.Get());
        run(l.Get());
    }

    ComPtr<ID3D12RootSignature> root_signature(const D3D12_ROOT_PARAMETER1 *parameters = nullptr, UINT count = 0,
                                               const D3D12_STATIC_SAMPLER_DESC *samplers = nullptr, UINT num_samplers = 0,
                                               D3D12_ROOT_SIGNATURE_FLAGS flags =
                                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = count;
        desc.Desc_1_1.pParameters = parameters;
        desc.Desc_1_1.NumStaticSamplers = num_samplers;
        desc.Desc_1_1.pStaticSamplers = samplers;
        desc.Desc_1_1.Flags = flags;
        ComPtr<ID3DBlob> blob, error;
        CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, blob.GetAddressOf(), error.GetAddressOf()));
        ComPtr<ID3D12RootSignature> signature;
        CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(signature.GetAddressOf())));
        return signature;
    }

    ComPtr<ID3D12PipelineState> graphics_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc)
    {
        ComPtr<ID3D12PipelineState> pso;
        CHECK_HR(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf())));
        return pso;
    }

    ComPtr<ID3D12PipelineState> compute_pso(ID3D12RootSignature *signature, D3D12_SHADER_BYTECODE cs)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = signature;
        desc.CS = cs;
        ComPtr<ID3D12PipelineState> pso;
        CHECK_HR(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf())));
        return pso;
    }

    // Reads `size` bytes of a buffer back through a readback heap.
    std::vector<uint8_t> read_buffer(ID3D12Resource *source, UINT64 size, UINT64 offset = 0)
    {
        ComPtr<ID3D12Resource> readback = buffer(D3D12_HEAP_TYPE_READBACK, size);
        run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(readback.Get(), 0, source, offset, size); });
        std::vector<uint8_t> bytes(size);
        void *mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        std::memcpy(bytes.data(), mapped, size);
        readback->Unmap(0, nullptr);
        return bytes;
    }

    // Reads one subresource of a texture back, whatever its format size (rows tightly packed).
    // `bytes_per_pixel` is the size of a texel (or block for compressed formats).
    Image read_texture(ID3D12Resource *source, UINT subresource, UINT bytes_per_pixel)
    {
        const D3D12_RESOURCE_DESC desc = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 row_size = 0, total = 0;
        device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &row_size, &total);
        ComPtr<ID3D12Resource> readback = buffer(D3D12_HEAP_TYPE_READBACK, total);
        run([&](ID3D12GraphicsCommandList *l) {
            const D3D12_TEXTURE_COPY_LOCATION dst = footprint_location(readback.Get(), footprint);
            const D3D12_TEXTURE_COPY_LOCATION src = subresource_location(source, subresource);
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        });
        Image image;
        image.width = static_cast<UINT>(row_size / bytes_per_pixel);
        image.height = rows;
        image.depth = footprint.Footprint.Depth;
        image.bytes_per_pixel = bytes_per_pixel;
        image.data.resize(size_t(row_size) * rows * image.depth);
        uint8_t *mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        for (UINT z = 0; z < image.depth; ++z) {
            for (UINT y = 0; y < rows; ++y) {
                std::memcpy(image.data.data() + (size_t(z) * rows + y) * row_size,
                            mapped + footprint.Offset + (size_t(z) * rows + y) * footprint.Footprint.RowPitch, row_size);
            }
        }
        readback->Unmap(0, nullptr);
        return image;
    }

    // Uploads tightly packed data into one subresource of `target` through an upload buffer.
    void upload_texture(ID3D12Resource *target, UINT subresource, const void *data, UINT bytes_per_pixel)
    {
        const D3D12_RESOURCE_DESC desc = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 row_size = 0, total = 0;
        device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &row_size, &total);
        (void)bytes_per_pixel;
        ComPtr<ID3D12Resource> upload = buffer(D3D12_HEAP_TYPE_UPLOAD, total);
        uint8_t *mapped = nullptr;
        CHECK_HR(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
        const uint8_t *source = static_cast<const uint8_t *>(data);
        for (UINT z = 0; z < footprint.Footprint.Depth; ++z) {
            for (UINT y = 0; y < rows; ++y) {
                std::memcpy(mapped + footprint.Offset + (size_t(z) * rows + y) * footprint.Footprint.RowPitch,
                            source + (size_t(z) * rows + y) * row_size, row_size);
            }
        }
        upload->Unmap(0, nullptr);
        run([&](ID3D12GraphicsCommandList *l) {
            const D3D12_TEXTURE_COPY_LOCATION dst = subresource_location(target, subresource);
            const D3D12_TEXTURE_COPY_LOCATION src = footprint_location(upload.Get(), footprint);
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        });
    }
};

} // namespace t12

using namespace t12;
