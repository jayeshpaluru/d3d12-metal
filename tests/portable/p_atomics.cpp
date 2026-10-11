// Interlocked operations on every kind of UAV (tests/shaders/atomics.hlsl): typed buffers (R32_SINT / R32_UINT, one
// view with a non-zero FirstElement), R32_UINT / R32_SINT texture UAVs, raw and structured buffers, all heavily
// contended (65536 threads on 61 slots), plus the return-value form, whose originals must be a permutation of 0..N-1.
#include <algorithm>
#include <climits>

#include "atomics_cs.h"
#include "t12.h"

namespace {

constexpr UINT kThreads = 65536;
constexpr UINT kSlots = 61;
constexpr UINT kViewOffset = 64;  // FirstElement of the second RWBuffer<int> view (256 bytes: any texture buffer alignment)
constexpr UINT kElements = 256;
constexpr UINT kCases = 7;

int g_failures = 0;

void report(const char *what, size_t mismatches, size_t total)
{
    std::printf("p_atomics: %-40s %zu / %zu mismatches\n", what, mismatches, total);
    if (mismatches)
        ++g_failures;
}

// The value the shader adds to the signed slots.
int signed_value(UINT tid) { return (tid & 1) ? int(tid % 7) + 1 : -int(tid % 5); }

std::vector<uint32_t> words(const std::vector<uint8_t> &bytes, size_t count)
{
    std::vector<uint32_t> out(count);
    std::memcpy(out.data(), bytes.data(), count * 4);
    return out;
}

} // namespace

int main()
{
    Gpu gpu;
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0);
    const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 1), descriptor_table(&range, 1)};
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3D12PipelineState> pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_atomics_cs));
    ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);

    ComPtr<ID3D12Resource> sint_a = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4, uav);
    ComPtr<ID3D12Resource> sint_b = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, (kViewOffset + kElements) * 4, uav);
    ComPtr<ID3D12Resource> uint_buf = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4, uav);
    ComPtr<ID3D12Resource> uint_tex = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_UINT, 64, 4, uav));
    ComPtr<ID3D12Resource> sint_tex = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_SINT, 64, 4, uav));
    ComPtr<ID3D12Resource> raw = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4, uav);
    ComPtr<ID3D12Resource> structured = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, kElements * 4, uav);
    ComPtr<ID3D12Resource> originals = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, UINT64(kCases) * kThreads * 4, uav);

    auto buffer_view = [&](UINT slot, ID3D12Resource *resource, DXGI_FORMAT format, UINT first, UINT count, UINT stride,
                           D3D12_BUFFER_UAV_FLAGS flags = D3D12_BUFFER_UAV_FLAG_NONE) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Format = format;
        desc.Buffer.FirstElement = first;
        desc.Buffer.NumElements = count;
        desc.Buffer.StructureByteStride = stride;
        desc.Buffer.Flags = flags;
        gpu.device->CreateUnorderedAccessView(resource, nullptr, &desc, gpu.cpu_handle(heap.Get(), slot));
    };
    auto texture_view = [&](UINT slot, ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = format;
        gpu.device->CreateUnorderedAccessView(resource, nullptr, &desc, gpu.cpu_handle(heap.Get(), slot));
    };
    buffer_view(0, sint_a.Get(), DXGI_FORMAT_R32_SINT, 0, kElements, 0);
    buffer_view(1, sint_b.Get(), DXGI_FORMAT_R32_SINT, kViewOffset, kElements, 0);
    buffer_view(2, uint_buf.Get(), DXGI_FORMAT_R32_UINT, 0, kElements, 0);
    texture_view(3, uint_tex.Get(), DXGI_FORMAT_R32_UINT);
    texture_view(4, sint_tex.Get(), DXGI_FORMAT_R32_SINT);
    buffer_view(5, raw.Get(), DXGI_FORMAT_R32_TYPELESS, 0, kElements, 0, D3D12_BUFFER_UAV_FLAG_RAW);
    buffer_view(6, structured.Get(), DXGI_FORMAT_UNKNOWN, 0, kElements, 4);
    buffer_view(7, originals.Get(), DXGI_FORMAT_UNKNOWN, 0, kCases * kThreads, 4);

    auto dispatch_modes = [&](UINT first_mode, UINT last_mode) {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
        for (UINT mode = first_mode; mode <= last_mode; ++mode) {
            list->SetComputeRoot32BitConstants(0, 1, &mode, 0);
            list->Dispatch(mode == 0 ? 4 : kThreads / 64, 1, 1);
            list->ResourceBarrier(1, &barrier);
        }
        gpu.run(list.Get());
    };
    dispatch_modes(0, 7);  // one dispatch per mode, a UAV barrier between them

    // ---- Signed add / min / max on typed buffers (the second view is offset by FirstElement) ----------------------
    {
        std::vector<int> add(kSlots, 0), lo(kSlots, INT_MAX), hi(kSlots, INT_MIN);
        for (UINT tid = 0; tid < kThreads; ++tid) {
            const UINT slot = tid % kSlots;
            const int extreme = int((tid * 7) % 1000) - 500;
            add[slot] += signed_value(tid);
            lo[slot] = std::min(lo[slot], extreme);
            hi[slot] = std::max(hi[slot], extreme);
        }
        const char *names[2] = {"RWBuffer<int> add/min/max", "RWBuffer<int> FirstElement=64 add/min/max"};
        const std::vector<uint32_t> data[2] = {words(gpu.read_buffer(sint_a.Get(), kElements * 4), kElements),
                                               words(gpu.read_buffer(sint_b.Get(), kElements * 4, kViewOffset * 4), kElements)};
        for (int v = 0; v < 2; ++v) {
            size_t bad = 0;
            for (UINT s = 0; s < kSlots; ++s) {
                bad += int(data[v][s]) != add[s];
                bad += int(data[v][64 + s]) != lo[s];
                bad += int(data[v][128 + s]) != hi[s];
            }
            report(names[v], bad, kSlots * 3);
        }
    }

    // ---- Unsigned add / or / compare-exchange on a typed buffer ----------------------------------------------------
    {
        const std::vector<uint32_t> w = words(gpu.read_buffer(uint_buf.Get(), kElements * 4), kElements);
        std::vector<uint32_t> add(kSlots, 0), mask(kSlots, 0), count(kSlots, 0);
        for (UINT tid = 0; tid < kThreads; ++tid) {
            add[tid % kSlots] += tid + 1;
            mask[tid % kSlots] |= 1u << (tid % 32);
            ++count[tid % kSlots];
        }
        size_t bad = 0;
        for (UINT s = 0; s < kSlots; ++s)
            bad += (w[s] != add[s]) + (w[64 + s] != mask[s]) + (w[128 + s] != count[s]);
        report("RWBuffer<uint> add/or/cmpxchg", bad, kSlots * 3);
    }

    // ---- Texture UAVs --------------------------------------------------------------------------------------------
    {
        const Image u = gpu.read_texture(uint_tex.Get(), 0, 4);
        const Image s = gpu.read_texture(sint_tex.Get(), 0, 4);
        std::vector<uint32_t> add(kSlots, 0), hi(kSlots, 0);
        std::vector<int> sadd(kSlots, 0);
        for (UINT tid = 0; tid < kThreads; ++tid) {
            const UINT slot = tid % kSlots;
            add[slot] += tid + 1;
            hi[slot] = std::max(hi[slot], (tid * 13) % 9973);
            sadd[slot] += signed_value(tid);
        }
        size_t bad_u = 0, bad_s = 0;
        for (UINT slot = 0; slot < kSlots; ++slot) {
            uint32_t v0, v1;
            int32_t sv;
            std::memcpy(&v0, u.at(slot, 0), 4);
            std::memcpy(&v1, u.at(slot, 1), 4);
            std::memcpy(&sv, s.at(slot, 0), 4);
            bad_u += (v0 != add[slot]) + (v1 != hi[slot]);
            bad_s += sv != sadd[slot];
        }
        report("RWTexture2D<uint> add/max", bad_u, kSlots * 2);
        report("RWTexture2D<int> add", bad_s, kSlots);
    }

    // ---- Controls: raw and structured buffers ---------------------------------------------------------------------
    {
        const std::vector<uint32_t> r = words(gpu.read_buffer(raw.Get(), kElements * 4), kElements);
        const std::vector<uint32_t> st = words(gpu.read_buffer(structured.Get(), kElements * 4), kElements);
        std::vector<uint32_t> add(kSlots, 0);
        for (UINT tid = 0; tid < kThreads; ++tid)
            add[tid % kSlots] += tid + 1;
        size_t bad_r = 0, bad_s = 0;
        for (UINT s = 0; s < kSlots; ++s) {
            bad_r += r[s] != add[s];
            bad_s += st[s] != add[s];
        }
        report("RWByteAddressBuffer add", bad_r, kSlots);
        report("RWStructuredBuffer<uint> add", bad_s, kSlots);
    }

    // ---- Return values: the originals of "add 1" are exactly 0..N-1 and the final value is N ---------------------
    {
        const std::vector<uint32_t> w = words(gpu.read_buffer(originals.Get(), UINT64(kCases) * kThreads * 4), size_t(kCases) * kThreads);
        const char *names[kCases] = {"original RWBuffer<int>", "original RWBuffer<int> FirstElement=64",
                                     "original RWBuffer<uint>", "original RWTexture2D<uint>",
                                     "original RWTexture2D<int>", "original RWByteAddressBuffer",
                                     "original RWStructuredBuffer<uint>"};
        for (UINT c = 0; c < kCases; ++c) {
            std::vector<uint8_t> seen(kThreads, 0);
            size_t bad = 0;
            for (UINT tid = 0; tid < kThreads; ++tid) {
                const uint32_t value = w[size_t(c) * kThreads + tid];
                if (value >= kThreads || seen[value]++)
                    ++bad;
            }
            report(names[c], bad, kThreads);
        }
        const uint32_t a = words(gpu.read_buffer(sint_a.Get(), 4, 200 * 4), 1)[0];
        const uint32_t b = words(gpu.read_buffer(sint_b.Get(), 4, (kViewOffset + 200) * 4), 1)[0];
        const uint32_t ub = words(gpu.read_buffer(uint_buf.Get(), 4, 200 * 4), 1)[0];
        const Image u = gpu.read_texture(uint_tex.Get(), 0, 4);
        uint32_t tex_total;
        std::memcpy(&tex_total, u.at(62, 2), 4);
        report("final counters", (a != kThreads) + (b != kThreads) + (ub != kThreads) + (tex_total != kThreads), 4);
    }

    // ---- Plain (non-atomic) typed stores and loads through the offset view ---------------------------------------
    {
        dispatch_modes(8, 8);
        const std::vector<uint32_t> a = words(gpu.read_buffer(sint_a.Get(), 256 * 4), 256);
        const std::vector<uint32_t> b = words(gpu.read_buffer(sint_b.Get(), 256 * 4, kViewOffset * 4), 256);
        size_t bad = 0;
        for (UINT i = 0; i < 256; ++i)
            bad += (b[i] != i * 3) + (a[i] != i * 3 + 1);
        report("RWBuffer<int> FirstElement=64 store/load", bad, 512);
    }

    if (g_failures) {
        std::printf("p_atomics: FAIL (%d cases)\n", g_failures);
        return 1;
    }
    std::printf("p_atomics: PASS\n");
    return 0;
}
