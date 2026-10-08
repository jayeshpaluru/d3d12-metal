// A compute shader (tests/shaders/probe.hlsl) that reads resources at coordinates given by the test and
// writes what it saw to a UAV buffer: tests check descriptors by looking at what a shader gets.
#pragma once

#include <cmath>

#include "probe_cs.h"
#include "t12.h"

namespace probe_support {

struct Float4 {
    float x, y, z, w;
};

void expect_float4(const char *what, Float4 got, Float4 want, float tolerance = 0.02f)
{
    auto close = [&](float a, float b) { return std::fabs(a - b) <= tolerance; };
    if (!close(got.x, want.x) || !close(got.y, want.y) || !close(got.z, want.z) || !close(got.w, want.w)) {
        std::fprintf(stderr, "%s: got (%.4f,%.4f,%.4f,%.4f), expected (%.4f,%.4f,%.4f,%.4f)\n", what, got.x, got.y,
                     got.z, got.w, want.x, want.y, want.z, want.w);
        std::fflush(stderr);
        std::exit(1);
    }
}

Float4 rgba8(int r, int g, int b, int a)
{
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

D3D12_SAMPLER_DESC sampler_desc(D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode)
{
    D3D12_SAMPLER_DESC s = {};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = mode;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MinLOD = 0;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    return s;
}

D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(D3D12_SRV_DIMENSION dimension, DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = format;
    d.ViewDimension = dimension;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    return d;
}

constexpr UINT kMaxProbes = 64;

// Runs probe.hlsl over a set of resources and samplers.
struct Probe {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> srv_heap, sampler_heap;
    ComPtr<ID3D12Resource> results, params;
    uint8_t *mapped_params = nullptr;

    explicit Probe(Gpu &g) : gpu(g)
    {
        const D3D12_DESCRIPTOR_RANGE1 srv_range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0);
        const D3D12_DESCRIPTOR_RANGE1 sampler_range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
        const D3D12_DESCRIPTOR_RANGE1 uav_range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
        const D3D12_ROOT_PARAMETER1 parameters[] = {
            descriptor_table(&srv_range, 1),
            descriptor_table(&sampler_range, 1),
            root_descriptor(D3D12_ROOT_PARAMETER_TYPE_CBV, 0),
            root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0),
            descriptor_table(&uav_range, 1),
        };
        signature = gpu.root_signature(parameters, 5, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_probe_cs));
        srv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
        sampler_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, true);
        results = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kMaxProbes * sizeof(Float4), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        params = gpu.buffer(D3D12_HEAP_TYPE_UPLOAD, 2048);
        CHECK_HR(params->Map(0, nullptr, reinterpret_cast<void **>(&mapped_params)));
        clear_views();
        set_sampler(sampler_desc(D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
    }

    // Null descriptors of every shape the shader declares.
    void clear_views()
    {
        set_srv(0, nullptr, srv_desc(D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM));
        set_srv(1, nullptr, srv_desc(D3D12_SRV_DIMENSION_TEXTURE2DARRAY, DXGI_FORMAT_R8G8B8A8_UNORM));
        set_srv(2, nullptr, srv_desc(D3D12_SRV_DIMENSION_TEXTURECUBE, DXGI_FORMAT_R8G8B8A8_UNORM));
        set_srv(3, nullptr, srv_desc(D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM));
        set_srv(4, nullptr, srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_UINT));
        D3D12_SHADER_RESOURCE_VIEW_DESC raw = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32_TYPELESS);
        raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        set_srv(5, nullptr, raw);
        D3D12_SHADER_RESOURCE_VIEW_DESC structured = srv_desc(D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN);
        structured.Buffer.StructureByteStride = 16;
        set_srv(6, nullptr, structured);
    }

    void set_srv(UINT slot, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC &desc)
    {
        gpu.device->CreateShaderResourceView(resource, &desc, gpu.cpu_handle(srv_heap.Get(), slot));
    }

    void set_sampler(const D3D12_SAMPLER_DESC &desc)
    {
        gpu.device->CreateSampler(&desc, gpu.cpu_handle(sampler_heap.Get(), 0));
    }

    std::vector<Float4> run(UINT mode, const std::vector<Float4> &coords)
    {
        CHECK(coords.size() <= kMaxProbes);
        const UINT header[4] = {mode, static_cast<UINT>(coords.size()), 0, 0};
        std::memcpy(mapped_params, header, sizeof(header));
        std::memcpy(mapped_params + 16, coords.data(), coords.size() * sizeof(Float4));
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            ID3D12DescriptorHeap *heaps[] = {srv_heap.Get(), sampler_heap.Get()};
            list->SetDescriptorHeaps(2, heaps);
            list->SetComputeRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            list->SetComputeRootDescriptorTable(0, gpu.gpu_handle(srv_heap.Get(), 0));
            list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(sampler_heap.Get(), 0));
            list->SetComputeRootConstantBufferView(2, params->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
            list->SetComputeRootDescriptorTable(4, gpu.gpu_handle(srv_heap.Get(), 7));
            list->Dispatch(1, 1, 1);
        });
        const std::vector<uint8_t> bytes = gpu.read_buffer(results.Get(), coords.size() * sizeof(Float4));
        std::vector<Float4> out(coords.size());
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return out;
    }

    Float4 one(UINT mode, Float4 coord) { return run(mode, {coord})[0]; }
};

} // namespace probe_support

using namespace probe_support;
