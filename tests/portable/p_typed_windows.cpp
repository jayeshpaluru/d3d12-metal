// Typed buffer views with more than 2^28 elements (tests/shaders/typed_windows.hlsl). Metal texture buffers hold at
// most 2^28 elements, so the layer gives such views extra "window" descriptors and routes loads, stores and atomics
// with larger indices through them. Checked here with exact values on R32_UINT (UAV, SRV, atomics), R16_UINT,
// R8G8B8A8_UNORM and R10G10B10A2_UNORM views of buffers up to 2.1 GB, a view with FirstElement > 0, descriptors
// copied from a CPU heap (CopyDescriptorsSimple and CopyDescriptors) and a slot overwritten by a small view.
#include <chrono>
#include <cmath>

#include "t12.h"
#include "typed_windows_cs.h"
#include "typed_windows_srv_cs.h"

namespace {

constexpr UINT64 kL = 1ull << 28;  // the texture buffer limit
constexpr UINT kThreads = 64 * 256;

int g_failures = 0;

void expect(const char *what, UINT64 index, UINT64 got, UINT64 want)
{
    if (got == want)
        return;
    std::printf("p_typed_windows: FAIL %s: element %llu got 0x%llx expected 0x%llx\n", what, (unsigned long long)index,
                (unsigned long long)got, (unsigned long long)want);
    ++g_failures;
}

void expect_float(const char *what, UINT64 index, float got, float want)
{
    if (std::fabs(got - want) <= 1.0f / 255.0f + 1e-4f)
        return;
    std::printf("p_typed_windows: FAIL %s: element %llu got %.5f expected %.5f\n", what, (unsigned long long)index, got,
                want);
    ++g_failures;
}

uint32_t word_at(const std::vector<uint8_t> &bytes, size_t i)
{
    uint32_t w;
    std::memcpy(&w, bytes.data() + i * 4, 4);
    return w;
}

struct Test {
    Gpu gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso, srv_only_pso;
    ComPtr<ID3D12DescriptorHeap> heap, cpu_heap;
    ComPtr<ID3D12Resource> results, lists;
    uint32_t *mapped_lists = nullptr;

    Test()
    {
        const D3D12_DESCRIPTOR_RANGE1 ranges[] = {descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0),
                                                  descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1)};
        const D3D12_ROOT_PARAMETER1 parameters[] = {
            root_constants(0, 4),
            descriptor_table(ranges, 2),
            root_descriptor(D3D12_ROOT_PARAMETER_TYPE_CBV, 1),
            root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0),
        };
        signature = gpu.root_signature(parameters, 4, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_typed_windows_cs));
        srv_only_pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_typed_windows_srv_cs));
        heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, true);
        cpu_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, false);
        results = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kThreads * 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        lists = gpu.buffer(D3D12_HEAP_TYPE_UPLOAD, 512);
        CHECK_HR(lists->Map(0, nullptr, reinterpret_cast<void **>(&mapped_lists)));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE gpu_slot(UINT i) { return gpu.cpu_handle(heap.Get(), i); }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(UINT i) { return gpu.cpu_handle(cpu_heap.Get(), i); }

    static D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(DXGI_FORMAT format, UINT64 first, UINT count)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = format;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement = first;
        d.Buffer.NumElements = count;
        return d;
    }

    // A null resource makes a null view.
    void srv(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *r, DXGI_FORMAT f, UINT64 first, UINT count)
    {
        const auto d = srv_desc(f, first, count);
        gpu.device->CreateShaderResourceView(r, &d, at);
    }
    void uav(D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *r, DXGI_FORMAT f, UINT64 first, UINT count)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Format = f;
        d.Buffer.FirstElement = first;
        d.Buffer.NumElements = count;
        gpu.device->CreateUnorderedAccessView(r, nullptr, &d, at);
    }
    // The table at `slot`: t0 (Buffer<uint>) and u1 (RWBuffer<uint>) view `r` when asked, t1 (Buffer<float4>) is null.
    void table(UINT slot, ID3D12Resource *r, DXGI_FORMAT f, UINT64 first, UINT count, bool with_srv, bool with_uav)
    {
        srv(gpu_slot(slot), with_srv ? r : nullptr, with_srv ? f : DXGI_FORMAT_R32_UINT, first, with_srv ? count : 0);
        srv(gpu_slot(slot + 1), nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0);
        uav(gpu_slot(slot + 2), with_uav ? r : nullptr, with_uav ? f : DXGI_FORMAT_R32_UINT, first, with_uav ? count : 0);
    }

    // One dispatch of 64 x 256 threads; returns `words` words of the results buffer.
    std::vector<uint8_t> dispatch(UINT mode, UINT slot, const std::vector<UINT> &indices, const std::vector<UINT> &values,
                                  size_t words, ID3D12PipelineState *state = nullptr)
    {
        CHECK(indices.size() <= 64 && values.size() <= 64);
        std::memset(mapped_lists, 0, 512);
        std::memcpy(mapped_lists, indices.data(), indices.size() * 4);
        std::memcpy(mapped_lists + 64, values.data(), values.size() * 4);
        const UINT constants[4] = {mode, static_cast<UINT>(indices.size()), 0, 0};
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(signature.Get());
            list->SetPipelineState(state ? state : pso.Get());
            list->SetComputeRoot32BitConstants(0, 4, constants, 0);
            list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), slot));
            list->SetComputeRootConstantBufferView(2, lists->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
            list->Dispatch(64, 1, 1);
            const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
            list->ResourceBarrier(1, &barrier);
        });
        return gpu.read_buffer(results.Get(), words * 4);
    }

    // Writes 4-byte words at byte offsets of `buffer` through an upload buffer.
    void put_words(ID3D12Resource *buffer, const std::vector<std::pair<UINT64, uint32_t>> &writes)
    {
        std::vector<uint32_t> data;
        for (const auto &w : writes)
            data.push_back(w.second);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), data.size() * 4);
        gpu.run([&](ID3D12GraphicsCommandList *list) {
            for (size_t i = 0; i < writes.size(); ++i)
                list->CopyBufferRegion(buffer, writes[i].first, upload.Get(), i * 4, 4);
        });
    }

    // Loads elements through the Buffer<uint> SRV (mode 1) or the RWBuffer<uint> UAV (mode 2) of the table at `slot`.
    void check_load(UINT mode, const char *what, UINT slot, const std::vector<UINT> &indices, const std::vector<UINT> &expected)
    {
        const std::vector<uint8_t> got = dispatch(mode, slot, indices, {}, indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
            expect(what, indices[i], word_at(got, i), expected[i]);
    }
    void check_srv(const char *what, UINT slot, const std::vector<UINT> &indices, const std::vector<UINT> &expected)
    {
        check_load(1, what, slot, indices, expected);
    }
    void check_uav(const char *what, UINT slot, const std::vector<UINT> &indices, const std::vector<UINT> &expected)
    {
        check_load(2, what, slot, indices, expected);
    }
    uint32_t cpu_word(ID3D12Resource *buffer, UINT64 element) { return word_at(gpu.read_buffer(buffer, 4, element * 4), 0); }
};

// ---- 1, 4, 5 and 6: R32_UINT over 300M elements ---------------------------------------------------------------------
void r32_cases(Test &t)
{
    const UINT64 n = 300000000;
    ComPtr<ID3D12Resource> buffer =
        t.gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, n * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const DXGI_FORMAT f = DXGI_FORMAT_R32_UINT;
    t.table(0, buffer.Get(), f, 0, static_cast<UINT>(n), true, true);

    // Stores through the UAV; three more slots are zeroed for the atomics below.
    const UINT64 stored[] = {5, kL - 1, kL, kL + 7, kL + (1u << 24), n - 1};
    const UINT64 atomic_slots[] = {kL + 100, kL - 100, n - 3};
    std::vector<UINT> indices, values, expected;
    for (int i = 0; i < 6; ++i) {
        indices.push_back(static_cast<UINT>(stored[i]));
        values.push_back(0xC0DE0001u + i * 0x111);
        expected.push_back(values.back());
    }
    const std::vector<UINT> read_indices = indices;
    for (UINT64 a : atomic_slots) {
        indices.push_back(static_cast<UINT>(a));
        values.push_back(0);
    }
    t.dispatch(0, 0, indices, values, 1);

    t.check_srv("R32 SRV load", 0, read_indices, expected);
    {
        const std::vector<uint8_t> got = t.dispatch(1, 0, read_indices, {}, read_indices.size(), t.srv_only_pso.Get());
        for (size_t i = 0; i < read_indices.size(); ++i)
            expect("R32 SRV load (a shader whose only typed buffers are SRVs)", read_indices[i], word_at(got, i), expected[i]);
    }
    t.check_uav("R32 UAV load", 0, read_indices, expected);
    for (int i = 0; i < 6; ++i)
        expect("R32 CPU readback", stored[i], t.cpu_word(buffer.Get(), stored[i]), expected[i]);

    // Atomics from 16384 threads: add 1 returning the original (a permutation), add 3, add (t & 7).
    {
        const std::vector<UINT> slots = {static_cast<UINT>(atomic_slots[0]), static_cast<UINT>(atomic_slots[1]),
                                         static_cast<UINT>(atomic_slots[2])};
        const std::vector<uint8_t> originals = t.dispatch(3, 0, slots, {}, kThreads);
        std::vector<uint8_t> seen(kThreads, 0);
        size_t bad = 0;
        for (UINT i = 0; i < kThreads; ++i) {
            const uint32_t v = word_at(originals, i);
            if (v >= kThreads || seen[v]++)
                ++bad;
        }
        expect("R32 InterlockedAdd originals (permutation violations)", atomic_slots[0], bad, 0);
        const std::vector<UINT> want = {kThreads, 3 * kThreads, kThreads / 8 * 28};
        t.check_srv("R32 atomic sums (SRV)", 0, slots, want);
        for (int i = 0; i < 3; ++i)
            expect("R32 atomic sums (CPU readback)", atomic_slots[i], t.cpu_word(buffer.Get(), atomic_slots[i]), want[i]);
    }

    // 4: a view starting at element 1024: view element e is buffer element 1024 + e.
    {
        const UINT64 first = 1024;
        const UINT count = static_cast<UINT>(n - first);
        t.put_words(buffer.Get(), {{first * 4, 0x44440000}, {(first + kL + 10) * 4, 0x44441234}, {(first + kL - 1) * 4, 0x4444AAAA}});
        t.table(4, buffer.Get(), f, first, count, true, true);
        const std::vector<UINT> view_indices = {0, static_cast<UINT>(kL + 10), static_cast<UINT>(kL - 1),
                                                count - 1,  // the last element: buffer element n - 1
                                                count, count + 5};  // out of range
        const std::vector<UINT> want = {0x44440000, 0x44441234, 0x4444AAAA, expected[5], 0, 0};
        t.check_srv("FirstElement=1024 SRV load", 4, view_indices, want);
        t.check_uav("FirstElement=1024 UAV load", 4, view_indices, want);
        // A store past the limit through the offset view lands at buffer element 1024 + kL + 20.
        t.dispatch(0, 4, {static_cast<UINT>(kL + 20)}, {0x5150}, 1);
        expect("FirstElement=1024 UAV store (CPU readback)", first + kL + 20, t.cpu_word(buffer.Get(), first + kL + 20), 0x5150);
    }

    // 5: descriptors made in a CPU heap and copied into the shader-visible heap.
    {
        t.srv(t.cpu_slot(0), buffer.Get(), f, 0, static_cast<UINT>(n));
        t.srv(t.cpu_slot(1), nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0);
        t.uav(t.cpu_slot(2), buffer.Get(), f, 0, static_cast<UINT>(n));
        t.gpu.device->CopyDescriptorsSimple(3, t.gpu_slot(8), t.cpu_slot(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const std::vector<UINT> probe_indices = {static_cast<UINT>(kL + 7), static_cast<UINT>(kL), static_cast<UINT>(n - 1),
                                                 5, static_cast<UINT>(kL - 1), static_cast<UINT>(kL + (1u << 24))};
        const std::vector<UINT> want = {expected[3], expected[2], expected[5], expected[0], expected[1], expected[4]};
        t.check_srv("CopyDescriptorsSimple SRV", 8, probe_indices, want);
        t.check_uav("CopyDescriptorsSimple UAV", 8, probe_indices, want);

        // Multi-range copy: three single-descriptor source ranges scattered in the CPU heap into two destination ranges.
        t.srv(t.cpu_slot(4), buffer.Get(), f, 0, static_cast<UINT>(n));
        t.srv(t.cpu_slot(9), nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0);
        t.uav(t.cpu_slot(6), buffer.Get(), f, 0, static_cast<UINT>(n));
        const D3D12_CPU_DESCRIPTOR_HANDLE dst[] = {t.gpu_slot(16), t.gpu_slot(17)};
        const UINT dst_sizes[] = {1, 2};
        const D3D12_CPU_DESCRIPTOR_HANDLE src[] = {t.cpu_slot(4), t.cpu_slot(9), t.cpu_slot(6)};
        const UINT src_sizes[] = {1, 1, 1};
        t.gpu.device->CopyDescriptors(2, dst, dst_sizes, 3, src, src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t.check_srv("CopyDescriptors SRV", 16, probe_indices, want);
        t.check_uav("CopyDescriptors UAV", 16, probe_indices, want);

        // An overlapping copy within one heap, one slot down: a windowed view whose old slot receives a view without
        // windows keeps its windows in its new slot.
        t.srv(t.cpu_slot(11), buffer.Get(), f, 0, static_cast<UINT>(n));
        t.srv(t.cpu_slot(12), nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0);
        t.uav(t.cpu_slot(13), buffer.Get(), f, 0, static_cast<UINT>(n));
        t.srv(t.cpu_slot(14), nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0);
        t.gpu.device->CopyDescriptorsSimple(4, t.cpu_slot(10), t.cpu_slot(11), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t.gpu.device->CopyDescriptorsSimple(3, t.gpu_slot(24), t.cpu_slot(10), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t.check_srv("overlapping copy SRV", 24, probe_indices, want);
        t.check_uav("overlapping copy UAV", 24, probe_indices, want);

        // Overwrite the windowed views in slots 16 and 18 with small views of another buffer.
        constexpr UINT kSmall = 1000;
        std::vector<UINT> small_data(kSmall);
        for (UINT i = 0; i < kSmall; ++i)
            small_data[i] = i * 7 + 1;
        ComPtr<ID3D12Resource> small = t.gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kSmall * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> upload = t.gpu.upload_buffer(small_data.data(), kSmall * 4);
        t.gpu.run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(small.Get(), 0, upload.Get(), 0, kSmall * 4); });
        t.srv(t.gpu_slot(16), small.Get(), f, 0, kSmall);
        t.uav(t.gpu_slot(18), small.Get(), f, 0, kSmall);
        // The old windows must be gone: reads past the end (including at windowed indices) are zero.
        const std::vector<UINT> small_indices = {0, 1, 500, kSmall - 1, kSmall, 5000, static_cast<UINT>(kL + 7), static_cast<UINT>(kL)};
        const std::vector<UINT> small_want = {1, 8, 3501, (kSmall - 1) * 7 + 1, 0, 0, 0, 0};
        t.check_srv("overwritten slot SRV", 16, small_indices, small_want);
        t.check_uav("overwritten slot UAV", 16, small_indices, small_want);

        // 6: a small view in a fresh table behaves the same.
        t.table(24, small.Get(), f, 0, kSmall, true, true);
        t.check_srv("small SRV", 24, small_indices, small_want);
        t.check_uav("small UAV", 24, small_indices, small_want);
    }
}

// ---- 2: R16_UINT over 1.06G elements (windows 1 to 3) -------------------------------------------------------------
void r16_case(Test &t)
{
    const UINT64 n = 1060000000;
    ComPtr<ID3D12Resource> buffer = t.gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, n * 2);
    const UINT64 pairs[] = {10, kL - 32, kL + 64, 2 * kL + 32, 3 * kL + 64, n - 2};  // even element indices
    std::vector<std::pair<UINT64, uint32_t>> writes;
    std::vector<UINT> indices, expected;
    for (int i = 0; i < 6; ++i) {
        const uint32_t lo = 0x1111 * (i + 1), hi = lo + 7;
        writes.push_back({pairs[i] * 2, lo | (hi << 16)});
        indices.push_back(static_cast<UINT>(pairs[i]));
        expected.push_back(lo);
        indices.push_back(static_cast<UINT>(pairs[i] + 1));
        expected.push_back(hi);
    }
    t.put_words(buffer.Get(), writes);
    t.table(0, buffer.Get(), DXGI_FORMAT_R16_UINT, 0, static_cast<UINT>(n), true, false);
    t.check_srv("R16 SRV load", 0, indices, expected);
}

// ---- 3: R8G8B8A8_UNORM and R10G10B10A2_UNORM over 280M elements ----------------------------------------------------
struct Marker {
    UINT64 element;
    uint32_t word;
    float want[4];
};

Marker rgba8(UINT64 element, int r, int g, int b, int a)
{
    return {element, uint32_t(r | g << 8 | b << 16 | a << 24), {r / 255.f, g / 255.f, b / 255.f, a / 255.f}};
}

Marker rgb10a2(UINT64 element, int r, int g, int b, int a)
{
    return {element, uint32_t(r | g << 10 | b << 20 | a << 30), {r / 1023.f, g / 1023.f, b / 1023.f, a / 3.f}};
}

void unorm_case(Test &t)
{
    const UINT64 n = 280000000;
    ComPtr<ID3D12Resource> buffer = t.gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, n * 4);
    const std::vector<Marker> eight = {rgba8(7, 255, 128, 0, 255), rgba8(kL - 1, 10, 20, 30, 40),
                                       rgba8(kL, 200, 100, 50, 255), rgba8(kL + 5, 1, 2, 3, 4),
                                       rgba8(n - 1, 0, 255, 17, 128)};
    const std::vector<Marker> ten = {rgb10a2(11, 0, 1023, 100, 1), rgb10a2(kL + 9, 1023, 512, 0, 3),
                                     rgb10a2(n - 2, 1, 2, 3, 2)};
    std::vector<std::pair<UINT64, uint32_t>> writes;
    for (const auto *set : {&eight, &ten})
        for (const Marker &m : *set)
            writes.push_back({m.element * 4, m.word});
    t.put_words(buffer.Get(), writes);

    auto check = [&](const char *what, DXGI_FORMAT format, const std::vector<Marker> &markers) {
        t.srv(t.gpu_slot(0), nullptr, DXGI_FORMAT_R32_UINT, 0, 0);
        t.srv(t.gpu_slot(1), buffer.Get(), format, 0, static_cast<UINT>(n));
        t.uav(t.gpu_slot(2), nullptr, DXGI_FORMAT_R32_UINT, 0, 0);
        std::vector<UINT> indices;
        for (const Marker &m : markers)
            indices.push_back(static_cast<UINT>(m.element));
        const std::vector<uint8_t> got = t.dispatch(4, 0, indices, {}, indices.size() * 4);
        for (size_t i = 0; i < markers.size(); ++i)
            for (int c = 0; c < 4; ++c) {
                float v;
                std::memcpy(&v, got.data() + (i * 4 + c) * 4, 4);
                expect_float(what, markers[i].element, v, markers[i].want[c]);
            }
    };
    check("R8G8B8A8_UNORM SRV load", DXGI_FORMAT_R8G8B8A8_UNORM, eight);
    check("R10G10B10A2_UNORM SRV load", DXGI_FORMAT_R10G10B10A2_UNORM, ten);
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    Test t;
    r32_cases(t);
    r16_case(t);
    unorm_case(t);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (g_failures) {
        std::printf("p_typed_windows: FAIL (%d checks, %.1fs)\n", g_failures, seconds);
        return 1;
    }
    std::printf("p_typed_windows: PASS (%.1fs)\n", seconds);
    return 0;
}
