// A render pass continues across a barrier only when that is provably safe (queue.mm, Replay::barrier): the barrier
// names nothing the pass renders to and no draw of the pass binds a UAV. Draws whose pixel shader wrote a UAV end the
// pass at the barrier, so the next draw (reading it from the vertex shader) sees the data. Counts the passes of a list
// through the backend and checks the pixels.
#include "color_ps.h"
#include "color_vs.h"
#include "d3d12/command_queue.h"
#include "portable/t12.h"
#include "rt_mismatch_ps.h"
#include "rt_mismatch_vs.h"
#include "uav_pass_ps.h"
#include "uav_pass_psw.h"
#include "uav_pass_vs.h"
#include "uav_pass_vsr.h"

namespace {

constexpr UINT kSize = 32;

struct Scene {
    Gpu gpu;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> color, write, read;
    ComPtr<ID3D12Resource> target, data, unrelated, vertices;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_VERTEX_BUFFER_VIEW vbv{};

    Scene()
    {
        const D3D12_ROOT_PARAMETER1 parameters[] = {
            root_constants(0, 4), root_descriptor(D3D12_ROOT_PARAMETER_TYPE_SRV, 0), root_descriptor(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
        signature = gpu.root_signature(parameters, 3);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        color = gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_uav_pass_vs), T12_SHADER(g_uav_pass_ps), layout, 1));
        write = gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_uav_pass_vs), T12_SHADER(g_uav_pass_psw), layout, 1));
        read = gpu.graphics_pso(graphics_pso_desc(signature.Get(), T12_SHADER(g_uav_pass_vsr), T12_SHADER(g_uav_pass_ps), layout, 1));
        target = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET);
        data = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        unrelated = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        rtv_heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        const float triangle[] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
        vertices = gpu.upload_buffer(triangle, sizeof(triangle));
        vbv = {vertices->GetGPUVirtualAddress(), sizeof(triangle), 12};
    }

    // A list that has the pass set up: targets, a clear, viewport.
    ComPtr<ID3D12GraphicsCommandList> begin()
    {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.list();
        list->SetGraphicsRootSignature(signature.Get());
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->SetGraphicsRootShaderResourceView(1, data->GetGPUVirtualAddress());
        list->SetGraphicsRootUnorderedAccessView(2, data->GetGPUVirtualAddress());
        return list;
    }

    void draw(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso, const float colour[4])
    {
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        list->DrawInstanced(3, 1, 0, 0);
    }

    // Runs the list and returns the number of render passes it took.
    uint64_t run(ID3D12GraphicsCommandList *list)
    {
        CHECK_HR(list->Close());
        auto *queue = static_cast<d3d12m::CommandQueue *>(gpu.queue.Get());
        const uint64_t before = mtlb_queue_render_pass_count(queue->handle());
        gpu.execute(list);
        gpu.wait_idle();
        return mtlb_queue_render_pass_count(queue->handle()) - before;
    }
};

}  // namespace

int main()
{
    Scene scene;
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};

    // A pixel shader writes a UAV; the next draw reads it in its vertex shader. Run many times: a barrier that left the
    // pass open would read stale data now and then.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    for (int round = 0; round < 30; ++round) {
        scene.gpu.run([&](ID3D12GraphicsCommandList *l) {  // reset the buffer
            const D3D12_RESOURCE_BARRIER to_dest = transition(scene.data.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
            l->ResourceBarrier(1, &to_dest);
            const UINT zero[4] = {};
            ComPtr<ID3D12Resource> upload = scene.gpu.upload_buffer(zero, sizeof(zero));
            l->CopyBufferRegion(scene.data.Get(), 0, upload.Get(), 0, 16);
            const D3D12_RESOURCE_BARRIER back = transition(scene.data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            l->ResourceBarrier(1, &back);
        });
        ComPtr<ID3D12GraphicsCommandList> list = scene.begin();
        scene.draw(list.Get(), scene.write.Get(), red);
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            uav_barrier(scene.data.Get()),
            transition(scene.data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, barriers);
        scene.draw(list.Get(), scene.read.Get(), red);
        const D3D12_RESOURCE_BARRIER back = transition(scene.data.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const uint64_t passes = scene.run(list.Get());
        CHECK_EQ(passes, 2);  // the barrier after unordered access ends the pass; the final one finds none open
        const Image image = scene.gpu.read_texture(scene.target.Get(), 0, 4);
        expect_pixel("vertex shader read of a pixel shader write", image.pixel(kSize / 2, kSize / 2), {0, 255, 0, 255});
        expect_pixel("(corner)", image.pixel(2, 2), {0, 255, 0, 255});
    }

    // A barrier on a resource the pass knows nothing about leaves the pass alone: both draws share one.
    {
        ComPtr<ID3D12GraphicsCommandList> list = scene.begin();
        scene.draw(list.Get(), scene.color.Get(), red);
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            uav_barrier(scene.unrelated.Get()),
            transition(scene.unrelated.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, barriers);
        scene.draw(list.Get(), scene.color.Get(), blue);
        CHECK_EQ(scene.run(list.Get()), 1);
        const Image image = scene.gpu.read_texture(scene.target.Get(), 0, 4);
        expect_pixel("after an unrelated barrier", image.pixel(kSize / 2, kSize / 2), {0, 0, 255, 255});
    }

    // A barrier on a render target of the pass ends it.
    {
        ComPtr<ID3D12GraphicsCommandList> list = scene.begin();
        scene.draw(list.Get(), scene.color.Get(), red);
        const D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(scene.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(scene.target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list->ResourceBarrier(2, barriers);
        scene.draw(list.Get(), scene.color.Get(), blue);
        CHECK_EQ(scene.run(list.Get()), 2);
    }

    // Two pipelines on the same two targets (a normalised one and an integer one): one writes both, the other only the
    // first. The integer target keeps its own view for the one that leaves it alone, so switching between them keeps the
    // pass (a view of the other kind for the unwritten target would split it).
    {
        const D3D12_ROOT_PARAMETER1 constants = root_constants(0, 4);
        ComPtr<ID3D12RootSignature> signature = scene.gpu.root_signature(&constants, 1);
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC one = graphics_pso_desc(signature.Get(), T12_SHADER(g_color_vs), T12_SHADER(g_color_ps), layout, 1);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC both =
            graphics_pso_desc(signature.Get(), T12_SHADER(g_rt_mismatch_vs), T12_SHADER(g_rt_mismatch_ps), layout, 1);
        for (auto *desc : {&one, &both}) {
            desc->NumRenderTargets = 2;
            desc->RTVFormats[1] = DXGI_FORMAT_R32_UINT;
        }
        ComPtr<ID3D12PipelineState> writes_one = scene.gpu.graphics_pso(one), writes_both = scene.gpu.graphics_pso(both);
        ComPtr<ID3D12Resource> ids = scene.gpu.texture(tex2d_desc(DXGI_FORMAT_R32_UINT, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        ComPtr<ID3D12DescriptorHeap> rtvs = scene.gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
        scene.gpu.device->CreateRenderTargetView(scene.target.Get(), nullptr, scene.gpu.cpu_handle(rtvs.Get(), 0));
        scene.gpu.device->CreateRenderTargetView(ids.Get(), nullptr, scene.gpu.cpu_handle(rtvs.Get(), 1));
        D3D12_CPU_DESCRIPTOR_HANDLE handles[2] = {scene.gpu.cpu_handle(rtvs.Get(), 0), scene.gpu.cpu_handle(rtvs.Get(), 1)};

        ComPtr<ID3D12GraphicsCommandList> list = scene.gpu.list();
        list->SetGraphicsRootSignature(signature.Get());
        const D3D12_VIEWPORT viewport = {0, 0, float(kSize), float(kSize), 0, 1};
        const D3D12_RECT scissor = {0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(2, handles, FALSE, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(handles[0], clear, 0, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &scene.vbv);
        const float colour[4] = {0.25f, 0.5f, 1.0f, 1.0f};
        list->SetGraphicsRoot32BitConstants(0, 4, colour, 0);
        for (int i = 0; i < 4; ++i) {
            list->SetPipelineState(i % 2 ? writes_both.Get() : writes_one.Get());
            list->DrawInstanced(3, 1, 0, 0);
        }
        CHECK_EQ(scene.run(list.Get()), 1);
        const Image image = scene.gpu.read_texture(scene.target.Get(), 0, 4);
        expect_pixel("mixed pipelines, colour", image.pixel(kSize / 2, kSize / 2), {64, 128, 255, 255}, 2);
        const Image id_image = scene.gpu.read_texture(ids.Get(), 0, 4);
        uint32_t id;
        std::memcpy(&id, id_image.at(kSize / 2, kSize / 2), 4);
        CHECK_EQ(id, 1);
    }
    std::printf("test_pass_merge: PASS\n");
    return 0;
}
