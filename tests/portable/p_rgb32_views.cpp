// Typed buffer SRVs of R32G32B32_UINT / _SINT / _FLOAT (tests/shaders/rgb32_views.hlsl). Metal has no 3-channel 32-bit
// formats, so the layer widens the elements into a shadow buffer at the start of every submit. Checked with exact
// values: a load of element i returns (x, y, z, 1) from byte 12 * (FirstElement + i), for views at FirstElement 0 and
// at 5; a later submit's copy into the source shows in the next dispatch; and a view whose source buffer is released
// does not break later submits.
#include <cmath>

#include "rgb32_views_cs.h"
#include "t12.h"

namespace {

constexpr UINT kElements = 64;
constexpr UINT kFirst = 5;
constexpr UINT kCount = 20;

int g_failures = 0;

// Element words are float bit patterns, so the same bytes are exact as uint, int and float.
std::vector<float> make_data(float base)
{
    std::vector<float> data(kElements * 3);
    for (UINT i = 0; i < kElements; ++i)
        for (UINT k = 0; k < 3; ++k)
            data[i * 3 + k] = base + float(i) * 0.5f + float(k) * 100.0f;
    return data;
}

uint32_t bits(float f)
{
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}

} // namespace

int main()
{
    Gpu gpu;
    const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
    const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1),
                                                root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(parameters, 3, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3D12PipelineState> pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_rgb32_views_cs));
    ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true);
    ComPtr<ID3D12Resource> results =
        gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> source = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 12);

    auto fill = [&](ID3D12Resource *target, const std::vector<float> &data) {
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), data.size() * 4);
        gpu.run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(target, 0, upload.Get(), 0, data.size() * 4); });
    };
    auto view = [&](UINT slot, ID3D12Resource *resource, DXGI_FORMAT format, UINT first, UINT count) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = format;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = first;
        d.Buffer.NumElements = count;
        gpu.device->CreateShaderResourceView(resource, &d, gpu.cpu_handle(heap.Get(), slot));
    };
    // Slots 0-2: the three formats from element 0; 3-5: from element kFirst; 6: a second buffer's view.
    const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_FLOAT};
    for (UINT f = 0; f < 3; ++f) {
        view(f, source.Get(), formats[f], 0, kElements);
        view(3 + f, source.Get(), formats[f], kFirst, kCount);
    }

    auto dispatch = [&](UINT mode, UINT table, UINT count) {
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(signature.Get());
            list->SetPipelineState(pso.Get());
            const UINT constants[4] = {mode, count, 0, 0};
            list->SetComputeRoot32BitConstants(0, 4, constants, 0);
            list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), table));
            list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
        });
        std::vector<uint32_t> out(count * 4);
        const std::vector<uint8_t> bytes = gpu.read_buffer(results.Get(), count * 16);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return out;
    };
    // Checks `count` loaded elements starting at element `first` of `data` for view format `mode`.
    auto check = [&](const char *what, UINT mode, UINT table, UINT first, UINT count, const std::vector<float> &data) {
        const std::vector<uint32_t> got = dispatch(mode, table, count);
        size_t bad = 0;
        for (UINT i = 0; i < count; ++i) {
            const uint32_t alpha = mode == 2 ? bits(1.0f) : 1u;
            for (UINT k = 0; k < 3; ++k)
                bad += got[i * 4 + k] != bits(data[(first + i) * 3 + k]);
            bad += got[i * 4 + 3] != alpha;
        }
        std::printf("p_rgb32_views: %-44s %zu / %u mismatches\n", what, bad, count * 4);
        g_failures += bad != 0;
    };

    std::vector<float> data = make_data(1.25f);
    fill(source.Get(), data);
    check("R32G32B32_UINT, FirstElement 0", 0, 0, 0, kElements, data);
    check("R32G32B32_SINT, FirstElement 0", 1, 0, 0, kElements, data);
    check("R32G32B32_FLOAT, FirstElement 0", 2, 0, 0, kElements, data);
    check("R32G32B32_UINT, FirstElement 5", 0, 3, kFirst, kCount, data);
    check("R32G32B32_SINT, FirstElement 5", 1, 3, kFirst, kCount, data);
    check("R32G32B32_FLOAT, FirstElement 5", 2, 3, kFirst, kCount, data);

    // The source changes in a later submit.
    data = make_data(-7.5f);
    fill(source.Get(), data);
    check("UINT after the source was rewritten", 0, 0, 0, kElements, data);
    check("FLOAT FirstElement 5 after the rewrite", 2, 3, kFirst, kCount, data);

    // A view of a buffer that is then released.
    {
        ComPtr<ID3D12Resource> doomed = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 12);
        fill(doomed.Get(), make_data(3.0f));
        view(6, doomed.Get(), DXGI_FORMAT_R32G32B32_UINT, 0, kElements);
        view(7, source.Get(), DXGI_FORMAT_R32G32B32_SINT, 0, kElements);
        view(8, source.Get(), DXGI_FORMAT_R32G32B32_FLOAT, 0, kElements);
        dispatch(0, 6, kElements);
    }
    check("views still work after another source died", 1, 0, 0, kElements, data);

    std::printf("p_rgb32_views: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
