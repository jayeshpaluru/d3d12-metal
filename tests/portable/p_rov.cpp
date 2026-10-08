// Rasterizer ordered views (tests/shaders/rov.hlsl). Many instances of a full-screen triangle update the same buffer
// elements and texels with an update that depends on the order (value = value * 31 + instance + 1). With a ROV the
// result must be the in-order one at every pixel; the same draw with a plain UAV shows what the hardware does without
// ordering. Prints the mismatches of both. The ROV ones must be zero for the layer to report ROVs as supported
// (feature_level=12_1, see docs/STATUS.md); `p_rov --report` does not fail on a mismatch.
#include <cstring>

#include "rov_ps.h"
#include "rov_psp.h"
#include "rov_pspt.h"
#include "rov_pst.h"
#include "rov_vs.h"
#include "t12.h"

namespace {

constexpr UINT kSize = 256;
constexpr UINT kInstances = 96;

size_t mismatches(const uint8_t *bytes, const std::vector<uint32_t> &expected)
{
    size_t wrong = 0;
    const uint32_t *got = reinterpret_cast<const uint32_t *>(bytes);
    for (size_t i = 0; i < expected.size(); ++i)
        wrong += got[i] != expected[i];
    return wrong;
}

} // namespace

int main(int argc, char **argv)
{
    const bool report = argc > 1 && std::strcmp(argv[1], "--report") == 0;
    Gpu gpu;
    const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 2);
    const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0),
                                                root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 1), descriptor_table(&range, 1)};
    ComPtr<ID3D12RootSignature> signature = gpu.root_signature(parameters, 4, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    auto pso = [&](D3D12_SHADER_BYTECODE ps, D3D12_SHADER_BYTECODE vs) {
        return gpu.graphics_pso(graphics_pso_desc(signature.Get(), vs, ps, nullptr, 0));
    };
    ComPtr<ID3D12PipelineState> ordered_pso = pso(T12_SHADER(g_rov_ps), T12_SHADER(g_rov_vs));
    ComPtr<ID3D12PipelineState> plain_pso = pso(T12_SHADER(g_rov_psp), T12_SHADER(g_rov_vs));
    ComPtr<ID3D12PipelineState> ordered_texture_pso = pso(T12_SHADER(g_rov_pst), T12_SHADER(g_rov_vs));
    ComPtr<ID3D12PipelineState> plain_texture_pso = pso(T12_SHADER(g_rov_pspt), T12_SHADER(g_rov_vs));

    const UINT64 bytes = UINT64(kSize) * kSize * 4;
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    ComPtr<ID3D12Resource> ordered = gpu.committed(D3D12_HEAP_TYPE_DEFAULT, buffer_desc(bytes, uav), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> plain = gpu.committed(D3D12_HEAP_TYPE_DEFAULT, buffer_desc(bytes, uav), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> ordered_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_UINT, kSize, kSize, uav), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> plain_texture = gpu.texture(tex2d_desc(DXGI_FORMAT_R32_UINT, kSize, kSize, uav), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // (an application always has its descriptor heap set)
    ComPtr<ID3D12DescriptorHeap> descriptors = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, true);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R32_UINT;
    gpu.device->CreateUnorderedAccessView(ordered_texture.Get(), nullptr, &view, gpu.cpu_handle(descriptors.Get(), 0));
    gpu.device->CreateUnorderedAccessView(plain_texture.Get(), nullptr, &view, gpu.cpu_handle(descriptors.Get(), 1));

    uint32_t value = 0;
    for (UINT i = 0; i < kInstances; ++i)
        value = value * 31u + i + 1u;
    const std::vector<uint32_t> expected(size_t(kSize) * kSize, value);
    const std::vector<uint32_t> zeros(size_t(kSize) * kSize, 0);
    ComPtr<ID3D12Resource> zero_upload = gpu.upload_buffer(zeros.data(), bytes);

    size_t wrong[4] = {};  // ordered buffer, plain buffer, ordered texture, plain texture
    constexpr int kRounds = 10;
    for (int round = 0; round < kRounds; ++round) {
        gpu.upload_texture(ordered_texture.Get(), 0, zeros.data(), 4);
        gpu.upload_texture(plain_texture.Get(), 0, zeros.data(), 4);
        gpu.run([&](ID3D12GraphicsCommandList *l) {
            l->CopyBufferRegion(ordered.Get(), 0, zero_upload.Get(), 0, bytes);
            l->CopyBufferRegion(plain.Get(), 0, zero_upload.Get(), 0, bytes);
        });
        gpu.run([&](ID3D12GraphicsCommandList *l) {
            ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
            l->SetDescriptorHeaps(1, heaps);
            l->SetGraphicsRootSignature(signature.Get());
            const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
            const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
            l->RSSetViewports(1, &viewport);
            l->RSSetScissorRects(1, &scissor);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT constants[4] = {kSize, 0, 0, 0};
            l->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
            l->SetGraphicsRootUnorderedAccessView(1, ordered->GetGPUVirtualAddress());
            l->SetGraphicsRootUnorderedAccessView(2, plain->GetGPUVirtualAddress());
            l->SetGraphicsRootDescriptorTable(3, gpu.gpu_handle(descriptors.Get(), 0));
            const D3D12_RESOURCE_BARRIER barrier = uav_barrier(nullptr);
            for (ID3D12PipelineState *state : {ordered_pso.Get(), plain_pso.Get(), ordered_texture_pso.Get(), plain_texture_pso.Get()}) {
                l->SetPipelineState(state);
                l->DrawInstanced(3, kInstances, 0, 0);
                l->ResourceBarrier(1, &barrier);
            }
        });
        wrong[0] += mismatches(gpu.read_buffer(ordered.Get(), bytes).data(), expected);
        wrong[1] += mismatches(gpu.read_buffer(plain.Get(), bytes).data(), expected);
        wrong[2] += mismatches(gpu.read_texture(ordered_texture.Get(), 0, 4).data.data(), expected);
        wrong[3] += mismatches(gpu.read_texture(plain_texture.Get(), 0, 4).data.data(), expected);
    }
    const size_t total = expected.size() * kRounds;
    std::printf("p_rov: out of order (of %zu): ROV buffer %zu, plain UAV buffer %zu, ROV texture %zu, plain UAV texture %zu\n",
                total, wrong[0], wrong[1], wrong[2], wrong[3]);
    if (!report && (wrong[0] != 0 || wrong[2] != 0)) {
        std::fprintf(stderr, "p_rov: a rasterizer ordered view was not ordered\n");
        return 1;
    }
    return 0;
}
