// The DirectStorage GDeflate meta command: EnumerateMetaCommands / EnumerateMetaCommandParameters /
// CheckFeatureSupport(QUERY_META_COMMAND) / CreateMetaCommand / InitializeMetaCommand / ExecuteMetaCommand, the way
// DirectStorage 1.2 uses them. Streams are made with the reference compressor (gdeflate_ref.h) and decompressed on the GPU;
// the bytes are compared with the originals. Covers stored, fixed and dynamic Huffman blocks, long matches, a final
// partial tile, several streams per call, unaligned data, damaged streams (the kernels must stay inside their buffers)
// and the compute queue. `p_gdeflate --bench` prints the decompression throughput.
#include <chrono>
#include <cstring>
#include <random>
#include <string>

#include "gdeflate_ref.h"
#include "t12.h"

namespace {

const GUID kMetaCommandId = {0x1bddd090, 0xc47e, 0x459c, {0x8f, 0x81, 0x42, 0xc9, 0xf9, 0x7a, 0x53, 0x08}};

struct CreateArgs {
    UINT64 version, format, max_streams, flags;
};

struct ExecArgs {
    UINT64 input, input_size, output, output_size, control, control_size, scratch, scratch_size, stream_count, status,
        status_size;
};
static_assert(sizeof(ExecArgs) == 88, "DirectStorage's execution structure");

struct QueryIn {
    UINT16 version, streams;
    UINT32 reserved;
    UINT64 format;
};
struct QueryOut {
    UINT16 version, max_streams;
    UINT32 reserved;
    UINT64 scratch_size, reserved2;
};

std::vector<uint8_t> make_data(int kind, size_t size, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<uint8_t> d(size);
    switch (kind) {
    case 0:  // random: incompressible, stored blocks
        for (auto &b : d)
            b = static_cast<uint8_t>(rng());
        break;
    case 1:  // zeros: the longest matches
        break;
    case 2:  // a short text repeated: matches of every length, distance 20
        for (size_t i = 0; i < size; ++i)
            d[i] = static_cast<uint8_t>("the quick brown fox "[i % 20]);
        break;
    case 3: {  // words of a small dictionary: dynamic Huffman codes
        static const char *words[] = {"vertex", "texture", "normal", "buffer", "index", "mesh",
                                      "shader", "a",       "of",     "and",    "the",   "sample"};
        size_t i = 0;
        while (i < size) {
            for (const char *s = words[rng() % 12]; *s && i < size; ++s)
                d[i++] = static_cast<uint8_t>(*s);
            if (i < size)
                d[i++] = ' ';
        }
        break;
    }
    case 4:  // three symbols: distances all over the window
        for (size_t i = 0; i < size; ++i)
            d[i] = static_cast<uint8_t>(rng() % 3);
        break;
    default:  // runs of zeros between noise
        for (size_t i = 0; i < size; ++i)
            d[i] = (i / 100) & 1 ? static_cast<uint8_t>(rng()) : 0;
        break;
    }
    return d;
}

struct Fixture {
    Gpu &gpu;
    ComPtr<ID3D12Device5> device5;
    ComPtr<ID3D12MetaCommand> meta;
    UINT max_streams = 0;
    UINT64 scratch_size = 0;

    explicit Fixture(Gpu &g) : gpu(g)
    {
        CHECK_HR(gpu.device.As(&device5));

        // ---- Discovery, as DirectStorage does it ----
        UINT count = 0;
        CHECK_HR(device5->EnumerateMetaCommands(&count, nullptr));
        CHECK_EQ(count, 1);
        D3D12_META_COMMAND_DESC desc = {};
        count = 1;
        CHECK_HR(device5->EnumerateMetaCommands(&count, &desc));
        CHECK_EQ(count, 1);
        CHECK(desc.Id == kMetaCommandId);
        CHECK(desc.Name != nullptr && desc.Name[0] == L'D');
        CHECK(desc.ExecutionDirtyState & D3D12_GRAPHICS_STATE_COMPUTE_ROOT_SIGNATURE);

        struct Stage {
            D3D12_META_COMMAND_PARAMETER_STAGE stage;
            UINT count, size;
        };
        for (const Stage &s : {Stage{D3D12_META_COMMAND_PARAMETER_STAGE_CREATION, 4, 32},
                               Stage{D3D12_META_COMMAND_PARAMETER_STAGE_INITIALIZATION, 0, 0},
                               Stage{D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION, 11, 88}}) {
            UINT n = 0, size = 0;
            CHECK_HR(device5->EnumerateMetaCommandParameters(kMetaCommandId, s.stage, &size, &n, nullptr));
            CHECK_EQ(n, s.count);
            CHECK_EQ(size, s.size);
            std::vector<D3D12_META_COMMAND_PARAMETER_DESC> params(n);
            if (n) {
                CHECK_HR(device5->EnumerateMetaCommandParameters(kMetaCommandId, s.stage, &size, &n, params.data()));
                for (const auto &p : params) {
                    CHECK(p.Name != nullptr);
                    CHECK(p.StructureOffset % 8 == 0 && p.StructureOffset < s.size);
                    if (s.stage == D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION
                        && p.Type == D3D12_META_COMMAND_PARAMETER_TYPE_GPU_VIRTUAL_ADDRESS)
                        CHECK_EQ(p.RequiredResourceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            }
        }
        {
            GUID unknown = kMetaCommandId;
            unknown.Data1 ^= 1;
            UINT n = 0;
            CHECK(FAILED(device5->EnumerateMetaCommandParameters(unknown, D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,
                                                                 nullptr, &n, nullptr)));
        }

        // ---- The query that gives the scratch size ----
        QueryIn in = {1, 1024, 0, 1};
        QueryOut out = {};
        D3D12_FEATURE_DATA_QUERY_META_COMMAND query = {};
        query.CommandId = kMetaCommandId;
        query.pQueryInputData = &in;
        query.QueryInputDataSizeInBytes = sizeof(in);
        query.pQueryOutputData = &out;
        query.QueryOutputDataSizeInBytes = sizeof(out);
        CHECK_HR(gpu.device->CheckFeatureSupport(D3D12_FEATURE_QUERY_META_COMMAND, &query, sizeof(query)));
        CHECK_EQ(out.version, 1);
        CHECK_EQ(out.max_streams, 1024);
        CHECK(out.scratch_size >= 1024 * 4);
        max_streams = out.max_streams;
        scratch_size = out.scratch_size;
        query.QueryInputDataSizeInBytes = 4;  // too small
        CHECK(FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_QUERY_META_COMMAND, &query, sizeof(query))));
        query.CommandId.Data1 ^= 1;
        query.QueryInputDataSizeInBytes = sizeof(in);
        CHECK(FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_QUERY_META_COMMAND, &query, sizeof(query))));

        // ---- Creation ----
        CreateArgs args = {1, 1, max_streams, 0};
        CHECK_HR(device5->CreateMetaCommand(kMetaCommandId, 0, &args, sizeof(args), IID_PPV_ARGS(meta.GetAddressOf())));
        CHECK(meta->GetRequiredParameterResourceSize(D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION, 6) >= scratch_size);
        CreateArgs bad = {2, 1, 4, 0};
        ComPtr<ID3D12MetaCommand> none;
        CHECK(FAILED(device5->CreateMetaCommand(kMetaCommandId, 0, &bad, sizeof(bad), IID_PPV_ARGS(none.GetAddressOf()))));
        bad = {1, 2, 4, 0};
        CHECK(FAILED(device5->CreateMetaCommand(kMetaCommandId, 0, &bad, sizeof(bad), IID_PPV_ARGS(none.GetAddressOf()))));
        CHECK(FAILED(device5->CreateMetaCommand(kMetaCommandId, 0, &args, 8, IID_PPV_ARGS(none.GetAddressOf()))));
    }
};

// What a call needs apart from the streams themselves.
struct Options {
    size_t input_align = 4;       // alignment of the streams in the input buffer
    size_t gap = 0;               // bytes between the streams' outputs
    int declared = -1;            // the stream count in the control buffer (-1: all of them)
    int param_count = -1;         // StreamCount of the call (-1: all of them)
    bool compute_queue = false;
    int corrupt = -1;             // index of a stream whose compressed bytes are damaged (seed in `seed`)
    uint32_t seed = 1;
    bool break_header = false;    // the corrupted stream gets a bad id instead of damaged data
    bool bench = false;
};

constexpr uint8_t kGuard = 0xA5;

struct Result {
    double seconds = 0;
    size_t out_bytes = 0;
};

// Compresses the originals, runs the meta command and checks the output. Returns false when `corrupt` is set (the
// damaged stream's output is not checked, everything else is).
Result run(Fixture &f, const std::vector<std::vector<uint8_t>> &originals, int level, const Options &o = {})
{
    Gpu &gpu = f.gpu;
    const size_t n = originals.size();
    std::vector<std::vector<uint8_t>> packed;
    for (const auto &raw : originals)
        packed.push_back(gdeflate_ref::compress(raw.data(), raw.size(), level));
    if (o.corrupt >= 0) {
        std::vector<uint8_t> &p = packed[o.corrupt];
        std::mt19937 rng(o.seed);
        if (o.break_header) {
            p[1] ^= 0x55;
        } else {
            const size_t first = 8 + 4 * ((p[2] | (p[3] << 8)));  // the data after the tile table
            for (int i = 0; i < 40 && p.size() > first + 1; ++i)
                p[first + rng() % (p.size() - first)] ^= static_cast<uint8_t>(1 + rng() % 255);
        }
    }

    // Layout: streams in the input buffer at `input_align`, outputs one after the other with a guard on each side.
    constexpr size_t kGuardBytes = 64;
    std::vector<uint32_t> control(1 + 2 * n);
    control[0] = o.declared >= 0 ? o.declared : static_cast<uint32_t>(n);
    size_t in_size = o.input_align > 1 ? 0 : 1;
    size_t out_size = kGuardBytes;
    std::vector<size_t> in_at(n), out_at(n);
    for (size_t i = 0; i < n; ++i) {
        in_size = (in_size + o.input_align - 1) / o.input_align * o.input_align;
        in_at[i] = in_size;
        in_size += packed[i].size();
        out_at[i] = out_size;
        out_size += originals[i].size() + o.gap;
        control[1 + 2 * i] = static_cast<uint32_t>(in_at[i]);
        control[2 + 2 * i] = static_cast<uint32_t>(out_at[i]);
    }
    out_size += kGuardBytes;
    in_size += 256;  // the zero padding after the last stream

    std::vector<uint8_t> input(in_size, 0);
    for (size_t i = 0; i < n; ++i)
        std::memcpy(input.data() + in_at[i], packed[i].data(), packed[i].size());

    ComPtr<ID3D12Resource> input_buffer = gpu.upload_buffer(input.data(), input.size());
    ComPtr<ID3D12Resource> control_buffer = gpu.upload_buffer(control.data(), control.size() * 4);
    ComPtr<ID3D12Resource> output = gpu.committed(D3D12_HEAP_TYPE_DEFAULT, buffer_desc(out_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT scratch_streams = f.max_streams;
    ComPtr<ID3D12Resource> scratch = gpu.committed(
        D3D12_HEAP_TYPE_DEFAULT, buffer_desc(std::max<UINT64>(f.scratch_size, 4096), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(n <= scratch_streams);

    // The output starts as guard bytes (copied from an upload buffer).
    {
        std::vector<uint8_t> pattern(out_size, kGuard);
        ComPtr<ID3D12Resource> fill = gpu.upload_buffer(pattern.data(), pattern.size());
        gpu.run([&](ID3D12GraphicsCommandList *l) { l->CopyBufferRegion(output.Get(), 0, fill.Get(), 0, out_size); });
    }

    ExecArgs args = {};
    args.input = input_buffer->GetGPUVirtualAddress();
    args.input_size = input.size();
    args.output = output->GetGPUVirtualAddress();
    args.output_size = out_size;
    args.control = control_buffer->GetGPUVirtualAddress();
    args.control_size = control.size() * 4;
    args.scratch = scratch->GetGPUVirtualAddress();
    args.scratch_size = f.scratch_size;
    args.stream_count = o.param_count >= 0 ? static_cast<UINT64>(o.param_count) : n;

    Result result;
    const int repeats = o.bench ? 8 : 1;
    ComPtr<ID3D12CommandQueue> compute_queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (o.compute_queue) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        CHECK_HR(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(compute_queue.GetAddressOf())));
        CHECK_HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(allocator.GetAddressOf())));
        CHECK_HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())));
    } else {
        list = gpu.list();
    }
    ComPtr<ID3D12GraphicsCommandList4> list4;
    CHECK_HR(list.As(&list4));
    list4->InitializeMetaCommand(f.meta.Get(), nullptr, 0);
    for (int r = 0; r < repeats; ++r) {
        list4->ExecuteMetaCommand(f.meta.Get(), &args, sizeof(args));
        const D3D12_RESOURCE_BARRIER barrier = uav_barrier(output.Get());
        list4->ResourceBarrier(1, &barrier);
    }
    CHECK_HR(list4->Close());
    const auto start = std::chrono::steady_clock::now();
    if (o.compute_queue) {
        gpu.execute(list.Get(), compute_queue.Get());
        ComPtr<ID3D12Fence> fence;
        CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
        CHECK_HR(compute_queue->Signal(fence.Get(), 1));
        CHECK_HR(fence->SetEventOnCompletion(1, nullptr));
    } else {
        gpu.execute(list.Get());
        gpu.wait_idle();
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const std::vector<uint8_t> got = gpu.read_buffer(output.Get(), out_size);
    for (size_t i = 0; i < kGuardBytes; ++i) {
        CHECK_EQ(got[i], kGuard);
        CHECK_EQ(got[out_size - 1 - i], kGuard);
    }
    const size_t decompressed = std::min<size_t>(control[0], args.stream_count);
    for (size_t i = 0; i < n; ++i) {
        const size_t size = originals[i].size();
        const bool done = i < decompressed && static_cast<int>(i) != o.corrupt;
        if (done) {
            if (std::memcmp(got.data() + out_at[i], originals[i].data(), size) != 0) {
                size_t k = 0;
                while (got[out_at[i] + k] == originals[i][k])
                    ++k;
                std::fprintf(stderr, "stream %zu (%zu bytes, level %d): first difference at %zu: got %02x, expected %02x\n",
                             i, size, level, k, got[out_at[i] + k], originals[i][k]);
                std::exit(1);
            }
        } else if (static_cast<int>(i) != o.corrupt) {
            // not part of the call: untouched
            for (size_t k = 0; k < size; ++k)
                CHECK_EQ(got[out_at[i] + k], kGuard);
        }
        // the gap after a stream (and the bytes between) stays untouched
        for (size_t k = 0; k < o.gap; ++k)
            CHECK_EQ(got[out_at[i] + size + k], kGuard);
    }
    result.out_bytes = 0;
    for (const auto &raw : originals)
        result.out_bytes += raw.size();
    result.out_bytes *= repeats;
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    Gpu gpu;
    Fixture f(gpu);
    const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;

    if (bench) {
        // Throughput: 64 MB of each kind of data, eight decompressions per submit.
        for (int kind : {3, 4, 0}) {
            for (int level : {1, 6}) {
                std::vector<std::vector<uint8_t>> data = {make_data(kind, 64u << 20, 7)};
                Options o;
                o.bench = true;
                run(f, data, level, o);  // warm-up: compiles the kernels
                const Result r = run(f, data, level, o);
                std::printf("GDeflate kind %d level %2d: %.2f GB/s (%.1f ms for %zu MB of output)\n", kind, level,
                            r.out_bytes / r.seconds / 1e9, r.seconds * 1e3, r.out_bytes >> 20);
            }
        }
        return 0;
    }

    // ---- Every kind of data at every size that matters, at levels with stored, fixed and dynamic blocks ----
    const size_t sizes[] = {1, 31, 100, 4096, 65535, 65536, 65537, 150000, 1000003};
    for (int kind = 0; kind < 6; ++kind) {
        for (size_t size : sizes) {
            for (int level : {0, 1, 6, 12}) {
                if (size > 200000 && level > 6)
                    continue;  // (the slowest levels on big inputs only slow the test down)
                run(f, {make_data(kind, size, 100 + kind)}, level);
            }
        }
    }

    // ---- Several streams in one call, of different sizes, content and compression ----
    {
        std::vector<std::vector<uint8_t>> streams;
        for (int i = 0; i < 20; ++i)
            streams.push_back(make_data(i % 6, 1 + (i * 7919u) % 200000, 5 + i));
        run(f, streams, 6);
        run(f, streams, 1);
        Options o;
        o.gap = 3;  // outputs that start at odd offsets
        run(f, streams, 6, o);
        o = {};
        o.input_align = 1;  // streams at odd offsets
        run(f, streams, 6, o);
    }

    // ---- The control buffer's count and the call's count: the smaller of the two decompresses ----
    {
        std::vector<std::vector<uint8_t>> streams;
        for (int i = 0; i < 6; ++i)
            streams.push_back(make_data(3, 70000 + i, 9 + i));
        Options o;
        o.declared = 4;
        run(f, streams, 6, o);
        o = {};
        o.param_count = 2;
        run(f, streams, 6, o);
        o = {};
        o.declared = 0;
        run(f, streams, 6, o);
    }

    // ---- The most streams a call takes ----
    {
        std::vector<std::vector<uint8_t>> streams;
        for (UINT i = 0; i < f.max_streams; ++i)
            streams.push_back(make_data(static_cast<int>(i % 6), 1 + (i * 131u) % 5000, 3 + i));
        run(f, streams, 1);
    }

    // ---- The compute queue (DirectStorage's) ----
    {
        Options o;
        o.compute_queue = true;
        run(f, {make_data(3, 300000, 1), make_data(2, 70000, 2)}, 6, o);
    }

    // ---- Damaged streams: whatever they decode to, the rest of the output and the other streams are intact ----
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        std::vector<std::vector<uint8_t>> streams = {make_data(3, 100000, 1), make_data(3 + seed % 3, 140000, 2), make_data(2, 90000, 3)};
        Options o;
        o.corrupt = 1;
        o.seed = seed;
        run(f, streams, seed % 2 ? 6 : 1, o);
    }
    {
        std::vector<std::vector<uint8_t>> streams = {make_data(3, 100000, 1), make_data(3, 140000, 2), make_data(2, 90000, 3)};
        Options o;
        o.corrupt = 1;
        o.break_header = true;  // a stream that is not GDeflate is skipped
        run(f, streams, 6, o);
    }

    std::printf("p_gdeflate: ok\n");
    return 0;
}
