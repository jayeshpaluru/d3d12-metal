// Render passes are built by the backend from D3D12-shaped records: a clear
// before a draw folds into the pass, rebinding the same targets keeps the pass
// open, and a clear after a draw starts a new one.
#include "d3d12/command_queue.h"
#include "render_context.h"

namespace {

struct Scene {
    RenderContext ctx;
    Com<ID3D12RootSignature> signature;
    Com<ID3D12PipelineState> pso;
    Com<ID3D12Resource> vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> list;

    Scene()
    {
        D3D12_ROOT_PARAMETER1 param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = {0, 0, 4};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC1 rs_desc = {};
        rs_desc.NumParameters = 1;
        rs_desc.pParameters = &param;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        signature = ctx.create_root_signature(rs_desc);
        pso = ctx.create_color_pso(signature.get());

        // One triangle in each half of the target.
        const float vertices[] = {
            -0.9f, -0.9f, 0.0f, -0.5f, 0.9f, 0.0f, -0.1f, -0.9f, 0.0f,  // left
            0.1f,  -0.9f, 0.0f, 0.5f,  0.9f, 0.0f, 0.9f,  -0.9f, 0.0f,  // right
        };
        vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
        vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};

        CHECK_HR(ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
        CHECK_HR(ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), pso.get(),
                                               IID_PPV_ARGS(list.put())));
        list->SetGraphicsRootSignature(signature.get());
        ctx.set_viewport_and_scissor(list.get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
    }

    void draw(UINT first_vertex, const float color[4])
    {
        list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
        list->DrawInstanced(3, 1, first_vertex, 0);
    }

    // Closes the list, runs it and returns how many render passes it took.
    uint64_t run(Com<ID3D12Resource> *readback, UINT *row_pitch)
    {
        *readback = ctx.record_readback(list.get(), row_pitch);
        CHECK_HR(list->Close());
        auto *queue = static_cast<d3d12m::CommandQueue *>(ctx.queue.get());
        const uint64_t before = mtlb_queue_render_pass_count(queue->handle());
        ctx.execute_and_wait(list.get());
        return mtlb_queue_render_pass_count(queue->handle()) - before;
    }
};

} // namespace

int main()
{
    const float blue[4] = {0, 0, 1, 1}, black[4] = {0, 0, 0, 1};
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};

    {
        // Clear, bind, draw, bind the same targets again, draw: one pass.
        Scene scene;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.draw(0, red);
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.draw(3, green);

        Com<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("left", read_pixel(readback.get(), row_pitch, 16, 32), {255, 0, 0, 255});
        check_pixel("right", read_pixel(readback.get(), row_pitch, 48, 32), {0, 255, 0, 255});
        check_pixel("background", read_pixel(readback.get(), row_pitch, 32, 2), {0, 0, 255, 255});
    }

    {
        // A clear after a draw cannot join the pass: the first triangle is erased.
        Scene scene;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.draw(0, red);
        scene.list->ClearRenderTargetView(rtv, black, 0, nullptr);
        scene.draw(3, green);

        Com<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 2);
        check_pixel("erased", read_pixel(readback.get(), row_pitch, 16, 32), {0, 0, 0, 255});
        check_pixel("right", read_pixel(readback.get(), row_pitch, 48, 32), {0, 255, 0, 255});
    }

    {
        // A clear with no draw after it runs as a pass of its own before the copy.
        Scene scene;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);

        Com<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("cleared", read_pixel(readback.get(), row_pitch, 10, 10), {0, 0, 255, 255});
    }
    return 0;
}
