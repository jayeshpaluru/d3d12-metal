// Per-shader quirks (src/common/quirks.h): the hash, the built-in table, the configuration lists, and the effect of
// kQuirkForceComputeBarrier on a real pipeline: a barrier record after each dispatch (Dispatch and ExecuteIndirect)
// that the application did not record, seen through the backend's barrier counter.
#include <cstdlib>
#include <cstring>
#include <string>

#include "bridge/mtlb.h"
#include "fill_cs.h"
#include "t12.h"

// The layer's quirks code (it exports these two entry points for the test).
extern "C" uint64_t d3d12m_shader_hash(const void *code, size_t size);
extern "C" uint32_t d3d12m_lookup_quirks(const char *exe, uint64_t hash, const char *force_compute_barrier);

namespace {

constexpr uint32_t kForceBarrier = 1;   // d3d12m::kQuirkForceComputeBarrier

void unit_tests()
{
    // FNV-1 (not FNV-1a): vkd3d-proton's shader hash.
    CHECK(d3d12m_shader_hash("", 0) == 0xcbf29ce484222325ull);
    CHECK(d3d12m_shader_hash("a", 1) == 0xaf63bd4c8601b7beull);
    CHECK(d3d12m_shader_hash("foobar", 6) == 0x340d8765a4dda9c2ull);

    // The built-in entry for Spider-Man 2 is found for its executable (any case) and for no other.
    CHECK(d3d12m_lookup_quirks("Spider-Man2", 0x324071d329f05cccull, nullptr) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("spider-man2", 0x324071d329f05cccull, nullptr) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("Spider-Man", 0x324071d329f05cccull, nullptr) == 0);
    CHECK(d3d12m_lookup_quirks("Spider-Man2", 0x2222ull, nullptr) == 0);

    // The configuration: hex hashes, with or without 0x, separated by commas and spaces; works for any executable.
    const char *list = "0xABCD, 1234  deadbeefdeadbeef,0x5";
    CHECK(d3d12m_lookup_quirks("game", 0xabcd, list) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("game", 0x1234, list) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("game", 0xdeadbeefdeadbeefull, list) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("game", 0x5, list) == kForceBarrier);
    CHECK(d3d12m_lookup_quirks("game", 0x6, list) == 0);
    CHECK(d3d12m_lookup_quirks("game", 0xabcd, "") == 0);
    CHECK(d3d12m_lookup_quirks("game", 0xabcd, "xyz") == 0);   // an invalid list gives nothing
    CHECK(d3d12m_lookup_quirks("game", 0xabcd, nullptr) == 0);
}

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandSignature> dispatch_signature;
    ComPtr<ID3D12Resource> arguments, texture, structured, raw, appended, counter, float_texture, typed;

    explicit Fixture(Gpu &g) : gpu(g)
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_fill_cs));
        heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);

        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr UINT kSize = 16;
        constexpr UINT64 kCounterOffset = 4096;
        texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, uav));
        structured = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        raw = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        appended = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);
        counter = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kCounterOffset + 256, uav);
        float_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_FLOAT, kSize, kSize, uav));
        typed = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSize * kSize * 4, uav);

        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(structured.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 1));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        gpu.device->CreateUnorderedAccessView(raw.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 2));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = kSize * kSize;
        desc.Buffer.StructureByteStride = 4;
        desc.Buffer.CounterOffsetInBytes = kCounterOffset;
        gpu.device->CreateUnorderedAccessView(appended.Get(), counter.Get(), &desc, gpu.cpu_handle(heap.Get(), 3));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        gpu.device->CreateUnorderedAccessView(float_texture.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 4));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.Buffer.NumElements = kSize * kSize;
        gpu.device->CreateUnorderedAccessView(typed.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 5));

        // ExecuteIndirect of a dispatch: a signature with only the dispatch, one command in the buffer.
        D3D12_INDIRECT_ARGUMENT_DESC argument = {};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
        signature_desc.ByteStride = 12;
        signature_desc.NumArgumentDescs = 1;
        signature_desc.pArgumentDescs = &argument;
        CHECK_HR(gpu.device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(dispatch_signature.GetAddressOf())));
        const UINT command[3] = {2, 2, 1};
        arguments = gpu.upload_buffer(command, sizeof(command));
    }

    // Records and runs `dispatches` dispatches and `indirect` indirect dispatches; returns the barriers the backend saw.
    uint64_t run(int dispatches, int indirect)
    {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
        const UINT constants[4] = {16, 16, 0, 0};
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        mtlb_stats before;
        mtlb_stats_get(&before);
        for (int i = 0; i < dispatches; ++i)
            list->Dispatch(2, 2, 1);
        for (int i = 0; i < indirect; ++i)
            list->ExecuteIndirect(dispatch_signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
        gpu.run(list.Get());
        mtlb_stats after;
        mtlb_stats_get(&after);
        return after.barriers - before.barriers;
    }
};

} // namespace

int main(int argc, char **argv)
{
    unit_tests();

    // `test_quirks plain`: the quirk is not configured, no barrier appears. Otherwise the test configures it for the
    // shader it dispatches (by the hash of its bytecode) before the layer reads the configuration.
    const bool plain = argc > 1 && std::strcmp(argv[1], "plain") == 0;
    if (!plain) {
        char text[64];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(d3d12m_shader_hash(g_fill_cs, sizeof(g_fill_cs))));
        setenv("D3D12METAL_QUIRK_FORCE_COMPUTE_BARRIER", text, 1);
    }
    setenv("D3D12METAL_SHADER_HASHES", "1", 1);

    Gpu gpu;
    Fixture f(gpu);
    const uint64_t one = f.run(1, 0), three = f.run(3, 0), indirect = f.run(0, 2), both = f.run(2, 2);
    if (plain) {
        CHECK_EQ(one, 0);
        CHECK_EQ(three, 0);
        CHECK_EQ(indirect, 0);
        CHECK_EQ(both, 0);
    } else {
        CHECK_EQ(one, 1);
        CHECK_EQ(three, 3);
        CHECK_EQ(indirect, 2);
        CHECK_EQ(both, 4);
    }
    std::printf("test_quirks: ok (%s)\n", plain ? "no quirk" : "force compute barrier");
    return 0;
}
