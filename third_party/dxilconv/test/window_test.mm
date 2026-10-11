// SPDX-License-Identifier: LGPL-2.1-or-later
// Tests dxilconv_window_typed_buffers: rewrites DXC-compiled shaders, checks the output parses (dxc -dumpbin), converts
// it with Metal Shader Converter against a root signature holding the original and the window ranges and, for the GPU
// shader, runs it on a buffer with more than 2^28 elements. Build and run with test/run.sh.
//
//   window_test <dxc> <shader dir> <work dir>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#define IR_PRIVATE_IMPLEMENTATION
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "../dxilconv_c.h"

namespace {

constexpr uint32_t kSpaceOffset = 0x1000;
constexpr uint64_t kWindow = 1ull << 28;

int g_failures = 0;
std::string g_dxc, g_shaders, g_work;

void check(bool ok, const std::string &what)
{
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += !ok;
}

std::vector<uint8_t> read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_file(const std::string &path, const std::vector<uint8_t> &data)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), data.size());
}

std::string run(const std::string &cmd, int *status = nullptr)
{
    std::string out;
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof buf, p))
        out.append(buf, n);
    int rc = pclose(p);
    if (status)
        *status = rc;
    return out;
}

size_t count_of(const std::string &text, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

struct Range {
    IRDescriptorRangeType type;
    uint32_t space, base, count;
    bool typed;
};

// One descriptor table with all ranges (plus the windows of the typed ones when asked); returns it created, with the
// descriptor index of each range through `offsets`.
IRRootSignature *make_root_signature(const std::vector<Range> &ranges, bool with_windows, std::vector<uint32_t> *offsets,
                                     std::string *error_text)
{
    std::vector<IRDescriptorRange1> irs;
    std::vector<uint32_t> offs;
    uint32_t next = 0;
    auto add = [&](const Range &r) {
        irs.push_back({r.type, r.count, r.base, r.space, IRDescriptorRangeFlagNone, next});
        offs.push_back(next);
        next += r.count;
    };
    for (const Range &r : ranges)
        add(r);
    if (with_windows)
        for (const Range &r : ranges)
            if (r.typed)
                add({r.type, r.space + kSpaceOffset, 3 * r.base, 3 * r.count, true});
    IRRootParameter1 param = {};
    param.ParameterType = IRRootParameterTypeDescriptorTable;
    param.DescriptorTable = {uint32_t(irs.size()), irs.data()};
    param.ShaderVisibility = IRShaderVisibilityAll;
    IRVersionedRootSignatureDescriptor desc = {};
    desc.version = IRRootSignatureVersion_1_1;
    desc.desc_1_1 = {1, &param, 0, nullptr, IRRootSignatureFlagNone};
    IRError *error = nullptr;
    IRRootSignature *rs = IRRootSignatureCreateFromDescriptor(&desc, &error);
    if (!rs && error) {
        if (error_text)
            *error_text = "root signature error " + std::to_string(IRErrorGetCode(error));
        IRErrorDestroy(error);
    }
    if (offsets)
        *offsets = offs;
    return rs;
}

// Converts `dxil` with MSC; returns the metallib bytes (empty on failure).
std::vector<uint8_t> msc_convert(const std::vector<uint8_t> &dxil, IRRootSignature *rs, std::string *fn_name,
                                 std::string *error_text)
{
    IRCompiler *compiler = IRCompilerCreate();
    IRCompilerSetGlobalRootSignature(compiler, rs);
    IRCompilerSetCompatibilityFlags(compiler, IRCompatibilityFlagBoundsCheck);
    IRObject *input = IRObjectCreateFromDXIL(dxil.data(), dxil.size(), IRBytecodeOwnershipNone);
    IRError *error = nullptr;
    IRObject *output = IRCompilerAllocCompileAndLink(compiler, "main", input, &error);
    std::vector<uint8_t> bytes;
    if (!output) {
        if (error_text) {
            *error_text = "MSC error " + std::to_string(error ? IRErrorGetCode(error) : 0);
            if (error && IRErrorGetPayload(error))
                *error_text += std::string(": ") + static_cast<const char *>(IRErrorGetPayload(error));
        }
        if (error)
            IRErrorDestroy(error);
    } else {
        IRMetalLibBinary *lib = IRMetalLibBinaryCreate();
        IRShaderStage stage = IRObjectGetMetalIRShaderStage(output);
        if (IRObjectGetMetalLibBinary(output, stage, lib)) {
            bytes.resize(IRMetalLibGetBytecodeSize(lib));
            IRMetalLibGetBytecode(lib, bytes.data());
        }
        IRShaderReflection *refl = IRShaderReflectionCreate();
        if (fn_name && IRObjectGetReflection(output, stage, refl))
            *fn_name = IRShaderReflectionGetEntryPointFunctionName(refl);
        IRShaderReflectionDestroy(refl);
        IRMetalLibBinaryDestroy(lib);
        IRObjectDestroy(output);
    }
    IRObjectDestroy(input);
    IRCompilerDestroy(compiler);
    return bytes;
}

struct Case {
    const char *file;
    const char *profile;
    std::vector<Range> ranges;
    bool rewritten;
    int window_blocks; // minimum number of branches
    const char *metadata = nullptr; // text the rewritten module's metadata must contain
};

const IRDescriptorRangeType SRV = IRDescriptorRangeTypeSRV, UAV = IRDescriptorRangeTypeUAV,
                            CBV = IRDescriptorRangeTypeCBV;

std::vector<Case> cases()
{
    std::vector<Case> out;
    for (const char *profile : {"cs_6_0", "cs_6_6"}) {
        out.push_back({"load_uniform", profile, {{SRV, 0, 3, 1, true}, {UAV, 0, 0, 1, false}, {CBV, 0, 0, 1, false}}, true, 2});
        out.push_back({"load_float4", profile, {{SRV, 1, 0, 1, true}, {SRV, 0, 5, 1, true}, {UAV, 0, 0, 1, false}}, true, 2});
        out.push_back({"store_atomic", profile, {{SRV, 0, 3, 1, true}, {UAV, 0, 1, 1, true}, {UAV, 0, 2, 1, true}}, true, 6});
        out.push_back({"array", profile, {{SRV, 0, 2, 4, true}, {UAV, 0, 0, 1, false}, {CBV, 0, 0, 1, false}}, true, 1});
        out.push_back({"raw_only", profile, {{SRV, 0, 0, 2, false}, {UAV, 0, 0, 1, false}}, false, 0});
        out.push_back({"flow", profile, {{SRV, 0, 0, 1, true}, {UAV, 0, 0, 1, true}, {UAV, 0, 1, 1, false}, {CBV, 0, 0, 1, false}}, true, 4});
        out.push_back({"unbounded", profile, {{SRV, 2, 4, 1000, true}, {UAV, 2, 2, 1, true}, {UAV, 0, 0, 1, false}, {CBV, 0, 0, 1, false}}, true, 2,
                       "!\"_window\", i32 4098, i32 12, i32 -1"});
        out.push_back({"mixed", profile, {{SRV, 0, 0, 1, false}, {SRV, 0, 1, 1, true}, {UAV, 0, 0, 1, false}, {UAV, 0, 1, 1, true}}, true, 2});
    }
    out.push_back({"vs", "vs_6_0", {{SRV, 0, 0, 1, true}}, true, 1});
    return out;
}

void test_case(const Case &c)
{
    std::string name = std::string(c.file) + "." + c.profile;
    std::printf("%s\n", name.c_str());
    std::string in_path = g_work + "/" + name + ".dxil", out_path = g_work + "/" + name + ".window.dxil";
    int status;
    std::string log = run(g_dxc + " -T " + c.profile + " -E main " + g_shaders + "/" + c.file + ".hlsl -Fo " + in_path, &status);
    check(status == 0, "dxc compiles");
    if (status) {
        std::printf("%s\n", log.c_str());
        return;
    }
    std::vector<uint8_t> dxil = read_file(in_path);

    void *out = nullptr;
    uint32_t out_size = 0;
    char diag[512];
    int rc = dxilconv_window_typed_buffers(dxil.data(), uint32_t(dxil.size()), kSpaceOffset, &out, &out_size, diag, sizeof diag);
    check(rc == (c.rewritten ? 1 : 0), "return code " + std::to_string(rc) + (diag[0] ? std::string(" (") + diag + ")" : ""));
    if (!c.rewritten) {
        check(out == nullptr, "no output for a shader without typed buffer access");
        std::string error;
        IRRootSignature *rs = make_root_signature(c.ranges, true, nullptr, &error);
        check(!msc_convert(dxil, rs, nullptr, &error).empty(), "unchanged shader converts: " + error);
        IRRootSignatureDestroy(rs);
        return;
    }
    if (rc != 1)
        return;
    std::vector<uint8_t> rewritten(static_cast<uint8_t *>(out), static_cast<uint8_t *>(out) + out_size);
    dxilconv_free(out);
    write_file(out_path, rewritten);

    std::string text = run(g_dxc + " -dumpbin " + out_path, &status);
    check(status == 0 && text.find("define void @") != std::string::npos, "dxc -dumpbin parses the output");
    size_t windows = count_of(text, "_window");
    bool uses_nonuniform = c.profile[3] == '6' && c.profile[5] == '6' ? text.find("i1 true)") != std::string::npos
                                                                      : text.find("i32 57,") != std::string::npos;
    check(windows > 0, "window resources declared (" + std::to_string(windows) + " mentions of _window)");
    check(uses_nonuniform, "window handles are created");
    size_t branches = count_of(text, "br i1");
    if (c.metadata)
        check(text.find(c.metadata) != std::string::npos, std::string("metadata has ") + c.metadata);
    check(branches >= size_t(c.window_blocks), "branches: " + std::to_string(branches));
    // The original container's other parts survive.
    auto parts = [](const std::vector<uint8_t> &blob) {
        std::vector<std::string> fourcc;
        uint32_t count = *reinterpret_cast<const uint32_t *>(&blob[28]);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t off = *reinterpret_cast<const uint32_t *>(&blob[32 + 4 * i]);
            fourcc.emplace_back(reinterpret_cast<const char *>(&blob[off]), 4);
        }
        return fourcc;
    };
    check(parts(dxil) == parts(rewritten), "container parts unchanged in kind and order");
    // PSV0 (runtime info size, runtime info, resource count, ...) lists one more resource per typed buffer.
    auto psv_resources = [](const std::vector<uint8_t> &blob) -> int {
        uint32_t count = *reinterpret_cast<const uint32_t *>(&blob[28]);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t off = *reinterpret_cast<const uint32_t *>(&blob[32 + 4 * i]);
            if (std::memcmp(&blob[off], "PSV0", 4) == 0) {
                const uint8_t *data = &blob[off + 8];
                return int(*reinterpret_cast<const uint32_t *>(data + 4 + *reinterpret_cast<const uint32_t *>(data)));
            }
        }
        return -1;
    };
    int typed = int(std::count_if(c.ranges.begin(), c.ranges.end(), [](const Range &r) { return r.typed; }));
    check(psv_resources(rewritten) == psv_resources(dxil) + typed,
          "PSV0 lists the window resources (" + std::to_string(psv_resources(dxil)) + " -> " + std::to_string(psv_resources(rewritten)) + ")");

    std::string error;
    std::vector<uint32_t> offsets;
    IRRootSignature *rs = make_root_signature(c.ranges, true, &offsets, &error);
    check(rs != nullptr, "root signature with window ranges: " + error);
    if (rs) {
        std::string msc_error;
        check(!msc_convert(rewritten, rs, nullptr, &msc_error).empty(), "MSC converts the rewritten shader " + msc_error);
        IRRootSignatureDestroy(rs);
    }
    // Idempotence guard: running the pass on its own output must not be needed, but must not crash either.
    void *again = nullptr;
    uint32_t again_size = 0;
    int rc2 = dxilconv_window_typed_buffers(rewritten.data(), uint32_t(rewritten.size()), kSpaceOffset, &again, &again_size, diag, sizeof diag);
    check(rc2 >= 0, "second run over the output does not fail (rc " + std::to_string(rc2) + ")");
    if (rc2 == 1)
        dxilconv_free(again);
}

// ---- GPU ----

void test_gpu(const char *profile)
{
    std::printf("gpu.%s\n", profile);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device");
    if (!device)
        return;
    const uint64_t n_elements = 3 * kWindow + 1024;
    const uint64_t bytes = n_elements * 4;
    if (bytes > device.maxBufferLength) {
        std::printf("  skip: max buffer length %llu is below %llu\n", (unsigned long long)device.maxBufferLength, (unsigned long long)bytes);
        return;
    }

    std::string name = std::string("gpu.") + profile;
    std::string in_path = g_work + "/" + name + ".dxil";
    int status;
    run(g_dxc + " -T " + profile + " -E main " + g_shaders + "/gpu.hlsl -Fo " + in_path, &status);
    check(status == 0, "dxc compiles");
    std::vector<uint8_t> dxil = read_file(in_path);
    void *out = nullptr;
    uint32_t out_size = 0;
    char diag[512];
    int rc = dxilconv_window_typed_buffers(dxil.data(), uint32_t(dxil.size()), kSpaceOffset, &out, &out_size, diag, sizeof diag);
    check(rc == 1, "rewritten");
    if (rc != 1)
        return;
    std::vector<uint8_t> windowed(static_cast<uint8_t *>(out), static_cast<uint8_t *>(out) + out_size);
    dxilconv_free(out);

    const std::vector<Range> ranges = {{SRV, 0, 0, 1, true}, {SRV, 0, 1, 1, false}, {UAV, 0, 0, 2, true}, {UAV, 0, 2, 2, false}};
    // Descriptor layout: ranges in order, then the windows: SRV t0 (3), UAV u0..u1 (6).

    id<MTLBuffer> big = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    check(big != nil, "allocated " + std::to_string(bytes >> 20) + " MiB buffer");
    if (!big)
        return;
    id<MTLBuffer> dst_big = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> acc_big = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    check(dst_big != nil && acc_big != nil, "allocated the dst and acc buffers");
    if (!dst_big || !acc_big)
        return;
    uint32_t *words = static_cast<uint32_t *>(big.contents);
    uint32_t *dst_words = static_cast<uint32_t *>(dst_big.contents);
    uint32_t *acc_words = static_cast<uint32_t *>(acc_big.contents);
    std::vector<uint32_t> indices;
    for (uint64_t base : {0ull, kWindow, 2 * kWindow, 3 * kWindow})
        for (int64_t d = -3; d <= 3; ++d)
            if (int64_t(base) + d >= 0 && base + d < n_elements)
                indices.push_back(uint32_t(base + d));
    indices.push_back(1000);
    indices.push_back(uint32_t(kWindow + 123456));
    indices.push_back(uint32_t(2 * kWindow + 7654321));
    indices.push_back(uint32_t(3 * kWindow + 1000));
    auto initial = [](uint32_t i) { return i * 7u + 1u; };
    auto acc_initial = [](uint32_t i) { return i * 3u + 5u; };

    const uint32_t count = uint32_t(indices.size());
    id<MTLBuffer> idx_buffer = [device newBufferWithBytes:indices.data() length:count * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out_buffer = [device newBufferWithLength:count * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> old_buffer = [device newBufferWithLength:count * 4 options:MTLResourceStorageModeShared];

    // Runs the shader (rewritten or not) and returns outv / olds.
    auto run_gpu = [&](const std::vector<uint8_t> &shader, bool with_windows, std::vector<uint32_t> &outv, std::vector<uint32_t> &olds) -> bool {
        for (uint32_t i : indices) {
            words[i] = initial(i);
            acc_words[i] = acc_initial(i);
            dst_words[i] = 0;
        }
        std::string error, fn;
        std::vector<uint32_t> offsets;
        IRRootSignature *rs = make_root_signature(ranges, with_windows, &offsets, &error);
        if (!rs) {
            std::printf("  %s\n", error.c_str());
            return false;
        }
        std::vector<uint8_t> lib_bytes = msc_convert(shader, rs, &fn, &error);
        IRRootSignatureDestroy(rs);
        if (lib_bytes.empty()) {
            std::printf("  %s\n", error.c_str());
            return false;
        }
        dispatch_data_t data = dispatch_data_create(lib_bytes.data(), lib_bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *ns_error = nil;
        id<MTLLibrary> lib = [device newLibraryWithData:data error:&ns_error];
        id<MTLFunction> function = [lib newFunctionWithName:[NSString stringWithUTF8String:fn.c_str()]];
        id<MTLComputePipelineState> pso = function ? [device newComputePipelineStateWithFunction:function error:&ns_error] : nil;
        if (!pso) {
            std::printf("  pipeline: %s\n", ns_error.localizedDescription.UTF8String);
            return false;
        }

        const uint32_t total_entries = 1 + 1 + 2 + 2 + (with_windows ? 3 + 6 : 0);
        id<MTLBuffer> table = [device newBufferWithLength:total_entries * sizeof(IRDescriptorTableEntry) options:MTLResourceStorageModeShared];
        id<MTLBuffer> top = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
        auto *entries = static_cast<IRDescriptorTableEntry *>(table.contents);
        std::vector<id<MTLTexture>> textures;
        auto texture_view = [&](id<MTLBuffer> buffer, uint64_t first, uint64_t n) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor new];
            td.textureType = MTLTextureTypeTextureBuffer;
            td.pixelFormat = MTLPixelFormatR32Uint;
            td.width = n;
            td.height = 1;
            td.storageMode = MTLStorageModeShared;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
            id<MTLTexture> t = [buffer newTextureWithDescriptor:td offset:first * 4 bytesPerRow:std::min<uint64_t>(n * 4, 1u << 30)];
            textures.push_back(t);
            return t;
        };
        auto typed_entry = [&](uint32_t slot, id<MTLBuffer> buffer, uint64_t first_element) {
            uint64_t n = std::min<uint64_t>(n_elements - first_element, kWindow);
            IRBufferView view = {};
            view.buffer = buffer;
            view.bufferOffset = first_element * 4;
            view.bufferSize = std::min<uint64_t>(n * 4, 0xffffffffull);
            view.textureBufferView = texture_view(buffer, first_element, n);
            view.typedBuffer = true;
            IRDescriptorTableSetBufferView(&entries[slot], &view);
        };
        auto plain_entry = [&](uint32_t slot, id<MTLBuffer> b) {
            IRBufferView view = {};
            view.buffer = b;
            view.bufferSize = b.length;
            IRDescriptorTableSetBufferView(&entries[slot], &view);
        };
        // Original ranges: t0 (src), t1 (idxs), u0 (dst), u1 (acc), u2 (outv), u3 (olds).
        typed_entry(0, big, 0);
        plain_entry(1, idx_buffer);
        typed_entry(2, dst_big, 0);
        typed_entry(3, acc_big, 0);
        plain_entry(4, out_buffer);
        plain_entry(5, old_buffer);
        if (with_windows) {
            for (uint32_t w = 1; w <= 3; ++w) {
                typed_entry(6 + w - 1, big, w * kWindow);      // src: t0 -> windows t0..t2
                typed_entry(9 + w - 1, dst_big, w * kWindow);  // dst: u0 -> windows u0..u2
                typed_entry(12 + w - 1, acc_big, w * kWindow); // acc: u1 -> windows u3..u5
            }
        }
        *static_cast<uint64_t *>(top.contents) = table.gpuAddress;

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:top offset:0 atIndex:kIRArgumentBufferBindPoint];
        for (id<MTLBuffer> b : {big, dst_big, acc_big})
            [enc useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [enc useResource:table usage:MTLResourceUsageRead];
        for (id<MTLBuffer> b : {idx_buffer, out_buffer, old_buffer})
            [enc useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        for (id<MTLTexture> t : textures)
            [enc useResource:t usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [enc dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            std::printf("  command buffer failed: %s\n", cb.error.localizedDescription.UTF8String);
            return false;
        }
        outv.assign(static_cast<uint32_t *>(out_buffer.contents), static_cast<uint32_t *>(out_buffer.contents) + count);
        olds.assign(static_cast<uint32_t *>(old_buffer.contents), static_cast<uint32_t *>(old_buffer.contents) + count);
        return true;
    };

    std::vector<uint32_t> outv, olds;
    bool ran = run_gpu(windowed, true, outv, olds);
    check(ran, "windowed shader ran on the GPU");
    if (ran) {
        int bad = 0;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t i = indices[k];
            bool good = outv[k] == initial(i) && olds[k] == acc_initial(i) && acc_words[i] == acc_initial(i) + 1 &&
                        dst_words[i] == (i ^ 0xABCDu);
            if (!good) {
                ++bad;
                std::printf("    index %u: read %u (want %u), atomic old %u (want %u), acc %u (want %u), dst %u (want %u)\n", i,
                            outv[k], initial(i), olds[k], acc_initial(i), acc_words[i], acc_initial(i) + 1, dst_words[i], i ^ 0xABCDu);
            }
        }
        check(bad == 0, "all " + std::to_string(count) + " indices (around 0, 2^28, 2^29, 3*2^28) read, written and atomically updated correctly");
    }
    // Baseline: the unrewritten shader loses everything at or above 2^28.
    std::vector<uint32_t> outv0, olds0;
    if (run_gpu(dxil, false, outv0, olds0)) {
        int lost = 0, wrong_low = 0;
        for (uint32_t k = 0; k < count; ++k) {
            if (indices[k] >= kWindow)
                lost += outv0[k] == 0;
            else
                wrong_low += outv0[k] != initial(indices[k]);
        }
        check(wrong_low == 0, "baseline reads below 2^28 are right");
        std::printf("  info: unrewritten shader reads zero for %d of the indices >= 2^28\n", lost);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <dxc> <shader dir> <work dir>\n", argv[0]);
        return 2;
    }
    g_dxc = argv[1];
    g_shaders = argv[2];
    g_work = argv[3];
    run("mkdir -p " + g_work);
    @autoreleasepool {
        for (const Case &c : cases())
            test_case(c);
        for (const char *profile : {"cs_6_0", "cs_6_6"})
            test_gpu(profile);
    }
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
