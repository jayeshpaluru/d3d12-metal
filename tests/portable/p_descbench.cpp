// Descriptor write throughput: how many CreateXxxView / CreateSampler / CopyDescriptors calls per second
// the front-end sustains. Games write tens of thousands of descriptors per frame, and under Wine a bridge
// crossing costs about 350 ns, so descriptor creation must be a memory write. Prints one line per kind
// ("descbench: <kind> <ns per descriptor>") and fails when a kind is implausibly slow (a bridge call per write).
#include <algorithm>
#include <chrono>

#include "t12.h"

namespace {

constexpr int kWrites = 200000;
constexpr UINT kSlots = 1024;

struct Bench {
    Gpu gpu;
    ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, true);
    ComPtr<ID3D12DescriptorHeap> staging = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, false);
    ComPtr<ID3D12DescriptorHeap> samplers = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, true);
    ComPtr<ID3D12DescriptorHeap> rtvs = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64);
    double worst_ns = 0;

    template <typename F>
    void measure(const char *name, F &&write, int writes_per_call = 1, double limit_ns = 1500)
    {
        const int calls = kWrites / writes_per_call;
        for (int i = 0; i < 1000; ++i)  // warm up (creates the Metal views once)
            write(i);
        // The best of three runs: a descheduled thread on a busy machine must not look like a slow path.
        double ns = 1e30;
        for (int run = 0; run < 3; ++run) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < calls; ++i)
                write(i);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            ns = std::min(ns, seconds * 1e9 / (double(calls) * writes_per_call));
        }
        std::printf("descbench: %-28s %8.1f ns/descriptor  %6.2f M/s\n", name, ns, 1e3 / ns);
        std::fflush(stdout);
        if (ns > worst_ns)
            worst_ns = ns;
        (void)limit_ns;
    }
};

} // namespace

int main()
{
    Bench b;
    Gpu &gpu = b.gpu;
    const UINT inc = gpu.increment(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = b.heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i % kSlots) * inc;
        return h;
    };

    ComPtr<ID3D12Resource> texture;
    D3D12_HEAP_PROPERTIES def = t12::heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC td = t12::tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                             1, 4);
    CHECK_HR(gpu.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(texture.GetAddressOf())));
    ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 1 << 20, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> upload = gpu.buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 16);

    b.measure("SRV texture 2D (4 mips)", [&](int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MostDetailedMip = i % 4;
        d.Texture2D.MipLevels = UINT(-1);
        gpu.device->CreateShaderResourceView(texture.Get(), &d, cpu(i));
    });
    b.measure("SRV structured buffer", [&](int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = (i % 64) * 16;
        d.Buffer.NumElements = 16;
        d.Buffer.StructureByteStride = 16;
        gpu.device->CreateShaderResourceView(buffer.Get(), &d, cpu(i));
    });
    b.measure("SRV raw buffer", [&](int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = (i % 64) * 16;
        d.Buffer.NumElements = 256;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        gpu.device->CreateShaderResourceView(buffer.Get(), &d, cpu(i));
    });
    b.measure("SRV typed buffer", [&](int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = (i % 64) * 16;
        d.Buffer.NumElements = 16;
        gpu.device->CreateShaderResourceView(buffer.Get(), &d, cpu(i));
    });
    b.measure("UAV buffer", [&](int i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = (i % 64) * 16;
        d.Buffer.NumElements = 16;
        d.Buffer.StructureByteStride = 16;
        gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &d, cpu(i));
    });
    b.measure("UAV texture 2D", [&](int i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = i % 4;
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &d, cpu(i));
    });
    b.measure("CBV", [&](int i) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC d = {};
        d.BufferLocation = upload->GetGPUVirtualAddress() + (i % 64) * 256;
        d.SizeInBytes = 256;
        gpu.device->CreateConstantBufferView(&d, cpu(i));
    });
    b.measure("null SRV", [&](int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        gpu.device->CreateShaderResourceView(nullptr, &d, cpu(i));
    });
    {
        const UINT sinc = gpu.increment(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        b.measure("sampler", [&](int i) {
            D3D12_SAMPLER_DESC d = {};
            d.Filter = (i & 1) ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            d.MaxAnisotropy = 1;
            d.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE h = b.samplers->GetCPUDescriptorHandleForHeapStart();
            h.ptr += size_t(i % 64) * sinc;
            gpu.device->CreateSampler(&d, h);
        });
    }
    const UINT rinc = gpu.increment(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    b.measure("RTV", [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = b.rtvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i % 64) * rinc;
        D3D12_RENDER_TARGET_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = i % 4;
        gpu.device->CreateRenderTargetView(texture.Get(), &d, h);
    });
    b.measure("CopyDescriptorsSimple (x64)", [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE src = b.staging->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dst = cpu((i * 64) % (kSlots - 64));
        gpu.device->CopyDescriptorsSimple(64, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }, 64);

    std::printf("descbench: worst %.1f ns/descriptor\n", b.worst_ns);
    // A bridge crossing is about 350 ns under Wine and still well below this natively, so only
    // something much slower than a few memory writes and a lookup is a regression worth failing on.
    CHECK(b.worst_ns < 250);
    std::printf("p_descbench: OK\n");
    return 0;
}
