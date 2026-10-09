// SPDX-License-Identifier: LGPL-2.1-or-later
// The on-disk shader cache: a second device (a fresh in-memory cache, as a second run of the application) is
// served from disk, damaged entries are rebuilt, D3D12METAL_CACHE=0 turns it off, and the size limit evicts.
//
//   test_shader_cache            cold run, warm run, corruption
//   test_shader_cache disabled   run with D3D12METAL_CACHE=0: nothing is written
//   test_shader_cache evict      run with D3D12METAL_CACHE_MAX_MB=1: the directory stays near the limit
//   test_shader_cache large      30000 files left by earlier runs: the first store does not wait for the directory listing
//
// The first two take the cache directory from D3D12METAL_CACHE_DIR; the test makes a fresh one itself when
// the variable is not set.
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "bridge/mtlb.h"
#include "fill_cs.h"
#include "instanced_ps.h"
#include "instanced_vs.h"
#include "test_util.h"

namespace {

ComPtr<ID3D12RootSignature> make_signature(ID3D12Device *device, int variant, bool compute)
{
    D3D12_ROOT_PARAMETER1 parameters[24] = {};
    D3D12_DESCRIPTOR_RANGE1 range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    UINT count = 0;
    auto constants = [&](UINT reg, UINT n) {
        D3D12_ROOT_PARAMETER1 &p = parameters[count++];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants = {reg, 0, n};
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    if (compute) {
        constants(0, 4);
        D3D12_ROOT_PARAMETER1 &t = parameters[count++];
        t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        t.DescriptorTable = {1, &range};
        t.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (int i = 0; i <= variant % 16; ++i)
        constants(10 + i, 1 + variant / 16);  // distinct blobs for up to 16 * several variants
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = count;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, blob.GetAddressOf(), error.GetAddressOf()));
    ComPtr<ID3D12RootSignature> signature;
    CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature.GetAddressOf())));
    return signature;
}

// Creates `count` graphics and compute pipelines on a fresh device; returns the seconds it took.
double create_pipelines(int count, int first = 0)
{
    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
    const auto start = std::chrono::steady_clock::now();
    for (int v = first; v < first + count; ++v) {
        ComPtr<ID3D12RootSignature> gs = make_signature(device.Get(), v, false), cs = make_signature(device.Get(), v, true);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
        g.pRootSignature = gs.Get();
        g.VS = {g_instanced_vs, sizeof(g_instanced_vs)};
        g.PS = {g_instanced_ps, sizeof(g_instanced_ps)};
        g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        for (auto &target : g.BlendState.RenderTarget) {
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
            target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        g.SampleMask = UINT_MAX;
        g.InputLayout = {layout, 3};
        g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        g.NumRenderTargets = 1;
        g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        g.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> gp, cp;
        CHECK_HR(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(gp.GetAddressOf())));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
        c.pRootSignature = cs.Get();
        c.CS = {g_fill_cs, sizeof(g_fill_cs)};
        CHECK_HR(device->CreateComputePipelineState(&c, IID_PPV_ARGS(cp.GetAddressOf())));
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string g_cleanup_dir;

mtlb_cache_stats stats()
{
    mtlb_cache_stats s;
    mtlb_cache_get_stats(&s);
    return s;
}

std::vector<std::string> entry_files(const std::string &dir)
{
    std::vector<std::string> files;
    if (DIR *top = opendir(dir.c_str())) {
        while (const dirent *shard = readdir(top)) {
            if (shard->d_name[0] == '.')
                continue;
            if (DIR *d = opendir((dir + "/" + shard->d_name).c_str())) {
                while (const dirent *e = readdir(d))
                    if (e->d_name[0] != '.')
                        files.push_back(dir + "/" + shard->d_name + "/" + e->d_name);
                closedir(d);
            }
        }
        closedir(top);
    }
    return files;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string dir;
    if (const char *env = std::getenv("D3D12METAL_CACHE_DIR")) {
        dir = env;
    } else {
        char path[] = "/tmp/d3d12metal-cache-test-XXXXXX";
        CHECK(mkdtemp(path));
        dir = path;
        setenv("D3D12METAL_CACHE_DIR", path, 1);
    }
    if (mode != "") {
        // These modes start from an empty directory of their own.
        dir += "/" + mode + "-" + std::to_string(getpid());
        setenv("D3D12METAL_CACHE_DIR", dir.c_str(), 1);
        if (mode == "disabled")
            setenv("D3D12METAL_CACHE", "0", 1);
        if (mode == "evict")
            setenv("D3D12METAL_CACHE_MAX_MB", "1", 1);
        if (mode == "large")
            setenv("D3D12METAL_CACHE_MAX_MB", "4096", 1);
    } else {
        dir += "/cold-warm-" + std::to_string(getpid());  // repeated runs may overlap: one directory per process
        setenv("D3D12METAL_CACHE_DIR", dir.c_str(), 1);
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    g_cleanup_dir = dir;
    std::atexit([] {
        std::error_code e;
        std::filesystem::remove_all(g_cleanup_dir, e);
    });  // a subdirectory of the test's own: always a cold start

    if (mode == "disabled") {
        create_pipelines(3);
        const mtlb_cache_stats s = stats();
        CHECK(s.hits == 0 && s.misses == 0 && s.writes == 0);
        CHECK(entry_files(dir).empty());
        std::printf("test_shader_cache disabled: OK\n");
        return 0;
    }

    if (mode == "large") {
        // A big cache directory left by earlier runs: the first store must not list it while holding up the caller
        // (that pass used to run inline under the cache's global lock).
        for (int i = 0; i < 30000; ++i) {
            char shard[8];
            std::snprintf(shard, sizeof(shard), "%02x", i % 256);
            std::filesystem::create_directories(dir + "/" + shard, ignored);
            std::ofstream(dir + "/" + shard + "/old-" + std::to_string(i) + ".d3mc", std::ios::binary) << "xxxxxxxxxxxxxxxx";
        }
        const double cold = create_pipelines(2);
        const auto t0 = std::chrono::steady_clock::now();
        const mtlb_cache_stats s = stats();  // settles the background pass, then lists the directory once more
        const double listing = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("large: creating pipelines took %.1f ms; listing the directory (settle + stats) took %.1f ms; %llu bytes\n",
                    cold * 1e3, listing * 1e3, (unsigned long long)s.bytes_on_disk);
        CHECK(s.writes > 0);
        if (listing > 0.05)  // on a machine where the listing is that quick there is nothing to tell apart
            CHECK(cold < listing / 2);
        create_pipelines(2);
        std::printf("test_shader_cache large: OK\n");
        return 0;
    }

    if (mode == "evict") {
        // Enough distinct shaders (root signatures) to exceed 1 MB.
        for (int round = 0; round < 6; ++round)
            create_pipelines(24, round * 24);
        const mtlb_cache_stats s = stats();
        std::printf("evict: %llu writes, %llu evicted, %llu bytes on disk\n", (unsigned long long)s.writes,
                    (unsigned long long)s.evicted, (unsigned long long)s.bytes_on_disk);
        CHECK(s.writes > 0);
        CHECK(s.evicted > 0);
        CHECK(s.bytes_on_disk <= (1u << 20));
        std::printf("test_shader_cache evict: OK\n");
        return 0;
    }

    constexpr int kVariants = 8;
    const double cold = create_pipelines(kVariants);
    const mtlb_cache_stats after_cold = stats();
    CHECK(after_cold.misses > 0);
    CHECK(after_cold.writes > 0);
    CHECK(after_cold.hits == 0);
    const size_t entries = entry_files(dir).size();
    CHECK(entries == after_cold.writes);

    // A second device: its in-memory caches are empty, the disk cache answers.
    const double warm = create_pipelines(kVariants);
    const mtlb_cache_stats after_warm = stats();
    CHECK(after_warm.hits >= after_cold.writes);
    CHECK(after_warm.writes == after_cold.writes);
    CHECK(after_warm.corrupt == 0);
    std::printf("pipelines for %d root signatures: cold %.1f ms, warm %.1f ms (%zu cache entries, %llu bytes)\n", kVariants,
                cold * 1e3, warm * 1e3, entries, (unsigned long long)after_warm.bytes_on_disk);

    // Damage every entry in a different way: truncated, garbled, emptied, extended, wrong magic.
    std::vector<std::string> files = entry_files(dir);
    for (size_t i = 0; i < files.size(); ++i) {
        std::fstream f(files[i], std::ios::in | std::ios::out | std::ios::binary);
        CHECK(f.good());
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        f.close();
        switch (i % 5) {
        case 0: CHECK(truncate(files[i].c_str(), size / 2) == 0); break;
        case 1: {
            std::fstream g(files[i], std::ios::in | std::ios::out | std::ios::binary);
            g.seekp(size - 8);
            g.write("garbage!", 8);
            break;
        }
        case 2: CHECK(truncate(files[i].c_str(), 0) == 0); break;
        case 3: {
            std::ofstream g(files[i], std::ios::app | std::ios::binary);
            g << "trailing junk";
            break;
        }
        default: {
            std::fstream g(files[i], std::ios::in | std::ios::out | std::ios::binary);
            g.write("XXXXXXXX", 8);
            break;
        }
        }
    }
    create_pipelines(kVariants);  // every entry is rejected and rebuilt, nothing fails
    const mtlb_cache_stats after_damage = stats();
    CHECK(after_damage.corrupt >= entries);
    CHECK(after_damage.writes >= after_warm.writes + entries);
    create_pipelines(kVariants);
    const mtlb_cache_stats repaired = stats();
    CHECK(repaired.corrupt == after_damage.corrupt);
    CHECK(repaired.hits >= after_damage.hits + entries);

    // Entries that are intact on disk (right header, key and checksum) but whose contents the converter or Metal
    // rejects: planted by replacing the payload of every entry and recomputing the checksum. They are discarded and
    // rebuilt like damaged ones, never fatal.
    {
        files = entry_files(dir);
        size_t planted = 0;
        for (const std::string &path : files) {
            std::ifstream in(path, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            // Header: magic[8] version[4] kind[4] key[32] first_size[8] second_size[8] payload_hash[16]
            constexpr size_t kHeader = 80;
            CHECK(bytes.size() >= kHeader);
            const std::string garbage = "this is not a metallib, nor reflection json";
            const uint64_t first_size = garbage.size(), second_size = 0;
            std::memcpy(&bytes[48], &first_size, 8);
            std::memcpy(&bytes[56], &second_size, 8);
            uint8_t digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(garbage.data(), static_cast<CC_LONG>(garbage.size()), digest);
            std::memcpy(&bytes[64], digest, 16);
            bytes.resize(kHeader);
            bytes.insert(bytes.end(), garbage.begin(), garbage.end());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            ++planted;
        }
        CHECK(planted == entries);
        const mtlb_cache_stats before = stats();
        create_pipelines(kVariants);  // converter or Metal rejects each planted entry: all are rebuilt
        const mtlb_cache_stats after = stats();
        CHECK(after.corrupt >= before.corrupt + entries);
        CHECK(after.writes >= before.writes + entries);
        create_pipelines(kVariants);
        CHECK(stats().hits >= after.hits + entries);
        CHECK(stats().corrupt == after.corrupt);
    }

    // No temporary files are left behind by the atomic writes.
    for (const std::string &f : entry_files(dir))
        CHECK(f.find(".tmp-") == std::string::npos);
    std::printf("test_shader_cache: OK\n");
    return 0;
}
