// PSO creation from many threads at once (games create thousands of pipelines on loader threads): eight
// threads create graphics and compute pipelines from overlapping sets of root signatures and shaders, so the
// same conversion is often requested concurrently. Every creation must succeed and the pipelines must work.
#include <atomic>
#include <thread>

#include "fill_cs.h"
#include "instanced_ps.h"
#include "instanced_vs.h"
#include "t12.h"

namespace {

constexpr int kThreads = 8;
constexpr int kPerThread = 12;
constexpr int kVariants = 6;  // distinct root signatures: the same shaders convert once per variant

ComPtr<ID3D12RootSignature> make_signature(ID3D12Device *device, int variant, bool compute)
{
    std::vector<D3D12_ROOT_PARAMETER1> parameters;
    const D3D12_DESCRIPTOR_RANGE1 range = t12::descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
    if (compute) {
        parameters.push_back(t12::root_constants(0, 4));
        parameters.push_back(t12::descriptor_table(&range, 1));
    }
    for (int i = 0; i <= variant; ++i)  // unused constants make each variant a different root signature
        parameters.push_back(t12::root_constants(10 + i, 1));
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.Flags = compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&desc, blob.GetAddressOf(), error.GetAddressOf()));
    ComPtr<ID3D12RootSignature> signature;
    CHECK_HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature.GetAddressOf())));
    return signature;
}

} // namespace

int main()
{
    Gpu gpu;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(kThreads * kPerThread * 3);
    std::atomic<int> failures{0};

    auto worker = [&](int thread) {
        const D3D12_INPUT_ELEMENT_DESC instanced_layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
        for (int j = 0; j < kPerThread; ++j) {
            // Threads start at different variants, so the same (shader, root signature) pairs are requested
            // by several threads within the same few milliseconds.
            const int variant = (thread + j) % kVariants;
            ComPtr<ID3D12RootSignature> graphics_signature = make_signature(gpu.device.Get(), variant, false);
            ComPtr<ID3D12RootSignature> compute_signature = make_signature(gpu.device.Get(), variant, true);
            const size_t base = size_t(thread * kPerThread + j) * 3;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC a = t12::graphics_pso_desc(
                graphics_signature.Get(), T12_SHADER(g_instanced_vs), T12_SHADER(g_instanced_ps), instanced_layout, 3);
            if (FAILED(gpu.device->CreateGraphicsPipelineState(&a, IID_PPV_ARGS(pipelines[base].GetAddressOf()))))
                failures++;
            // The same shaders for another render target format: a second pipeline, no second conversion.
            D3D12_GRAPHICS_PIPELINE_STATE_DESC b = t12::graphics_pso_desc(
                graphics_signature.Get(), T12_SHADER(g_instanced_vs), T12_SHADER(g_instanced_ps), instanced_layout, 3,
                DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (FAILED(gpu.device->CreateGraphicsPipelineState(&b, IID_PPV_ARGS(pipelines[base + 1].GetAddressOf()))))
                failures++;
            D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
            c.pRootSignature = compute_signature.Get();
            c.CS = T12_SHADER(g_fill_cs);
            if (FAILED(gpu.device->CreateComputePipelineState(&c, IID_PPV_ARGS(pipelines[base + 2].GetAddressOf()))))
                failures++;
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(worker, t);
    for (std::thread &t : threads)
        t.join();

    CHECK_EQ(failures.load(), 0);
    for (const auto &pso : pipelines)
        CHECK(pso != nullptr);

    // One of the pipelines made under contention renders: a triangle of instance colour at the centre.
    constexpr UINT kSize = 32;
    ComPtr<ID3D12RootSignature> signature = make_signature(gpu.device.Get(), 0, false);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}};
    ComPtr<ID3D12PipelineState> pso = gpu.graphics_pso(
        t12::graphics_pso_desc(signature.Get(), T12_SHADER(g_instanced_vs), T12_SHADER(g_instanced_ps), layout, 3));
    ComPtr<ID3D12Resource> target = gpu.texture(
        t12::tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    const float positions[] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};
    struct Instance {
        float offset[2];
        uint8_t color[4];
    } instance = {{0, 0}, {255, 0, 0, 255}};
    ComPtr<ID3D12Resource> positions_buffer = gpu.upload_buffer(positions, sizeof(positions));
    ComPtr<ID3D12Resource> instance_buffer = gpu.upload_buffer(&instance, sizeof(instance));
    const D3D12_VERTEX_BUFFER_VIEW views[2] = {{positions_buffer->GetGPUVirtualAddress(), sizeof(positions), 8},
                                               {instance_buffer->GetGPUVirtualAddress(), sizeof(instance), 12}};
    ComPtr<ID3D12Resource> readback;
    gpu.run([&](ID3D12GraphicsCommandList *list) {
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 2, views);
        list->DrawInstanced(3, 1, 0, 0);
    });
    const t12::Image image = gpu.read_texture(target.Get(), 0, 4);
    t12::expect_pixel("centre", image.pixel(kSize / 2, kSize / 2), {255, 0, 0, 255});
    t12::expect_pixel("corner", image.pixel(0, 0), {0, 0, 255, 255});

    // A render target handle that is not a descriptor binds nothing: the previous targets are not kept, so this draw
    // (red triangle) does not reach the target, which was cleared to green first.
    gpu.run([&](ID3D12GraphicsCommandList *list) {
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE bogus = {16};
        list->SetGraphicsRootSignature(signature.Get());
        list->SetPipelineState(pso.Get());
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float green[4] = {0, 1, 0, 1};
        list->ClearRenderTargetView(rtv, green, 0, nullptr);
        list->OMSetRenderTargets(1, &bogus, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 2, views);
        list->DrawInstanced(3, 1, 0, 0);
    });
    const t12::Image after = gpu.read_texture(target.Get(), 0, 4);
    t12::expect_pixel("centre after a draw with a bad target handle", after.pixel(kSize / 2, kSize / 2), {0, 255, 0, 255});
    std::printf("p_psothreads: OK (%zu pipelines)\n", pipelines.size());
    return 0;
}
