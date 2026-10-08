// Render passes are built by the backend from D3D12-shaped records: a clear
// before a draw folds into the pass, rebinding the same targets keeps the pass
// open, and a clear after a draw starts a new one.
#include "d3d12/command_queue.h"
#include "render_context.h"

namespace {

struct Scene {
    RenderContext ctx;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    ComPtr<ID3D12GraphicsCommandList> list;

    Scene()
    {
        CD3DX12_ROOT_PARAMETER1 param;
        param.InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        signature = ctx.create_root_signature(&param, 1);
        pso = ctx.create_color_pso(signature.Get());

        // One triangle in each half of the target.
        const float vertices[] = {
            -0.9f, -0.9f, 0.0f, -0.5f, 0.9f, 0.0f, -0.1f, -0.9f, 0.0f,  // left
            0.1f,  -0.9f, 0.0f, 0.5f,  0.9f, 0.0f, 0.9f,  -0.9f, 0.0f,  // right
        };
        vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
        vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};

        list = ctx.create_list(pso.Get());
        list->SetGraphicsRootSignature(signature.Get());
        ctx.set_viewport_and_scissor(list.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
    }

    void draw(UINT first_vertex, const float color[4])
    {
        list->SetGraphicsRoot32BitConstants(0, 4, color, 0);
        list->DrawInstanced(3, 1, first_vertex, 0);
    }

    // Closes the list, runs it and returns how many render passes it took.
    uint64_t run(ComPtr<ID3D12Resource> *readback, UINT *row_pitch)
    {
        *readback = ctx.record_readback(list.Get(), row_pitch);
        CHECK_HR(list->Close());
        auto *queue = static_cast<d3d12m::CommandQueue *>(ctx.queue.Get());
        const uint64_t before = mtlb_queue_render_pass_count(queue->handle());
        ctx.execute_and_wait(list.Get());
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

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("left", read_pixel(readback.Get(), row_pitch, 16, 32), {255, 0, 0, 255});
        check_pixel("right", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
        check_pixel("background", read_pixel(readback.Get(), row_pitch, 32, 2), {0, 0, 255, 255});
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

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 2);
        check_pixel("erased", read_pixel(readback.Get(), row_pitch, 16, 32), {0, 0, 0, 255});
        check_pixel("right", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
    }

    {
        // A clear with no draw after it runs as a pass of its own before the copy.
        Scene scene;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("cleared", read_pixel(readback.Get(), row_pitch, 10, 10), {0, 0, 255, 255});
    }

    {
        // Setting the same root signature again keeps the root arguments.
        Scene scene;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
        scene.list->SetGraphicsRootSignature(scene.signature.Get());
        scene.list->DrawInstanced(3, 1, 3, 0);

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("kept constants", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
    }

    {
        // Clearing a view that is not bound leaves the open pass alone and waits for a pass that binds the view or for a
        // reader: draw, clear the other texture, draw again are one pass, and the clear runs as a pass of its own when the
        // copy at the end needs the list's work done (two passes in all).
        Scene scene;
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> other;
        CHECK_HR(scene.ctx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                           IID_PPV_ARGS(other.ReleaseAndGetAddressOf())));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> other_heap;
        CHECK_HR(scene.ctx.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(other_heap.ReleaseAndGetAddressOf())));
        const D3D12_CPU_DESCRIPTOR_HANDLE other_rtv = other_heap->GetCPUDescriptorHandleForHeapStart();
        scene.ctx.device->CreateRenderTargetView(other.Get(), nullptr, other_rtv);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.draw(0, red);
        scene.list->ClearRenderTargetView(other_rtv, green, 0, nullptr);
        scene.draw(3, green);

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 2);
        check_pixel("before the clear", read_pixel(readback.Get(), row_pitch, 16, 32), {255, 0, 0, 255});
        check_pixel("after the clear", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
    }

    {
        // A view with DXGI_FORMAT_UNKNOWN still selects its mip: clear mip 1.
        RenderContext ctx;
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, kTargetSize, kTargetSize, 1, 2, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> texture;
        CHECK_HR(ctx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                     IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
        D3D12_RENDER_TARGET_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 1;
        ctx.device->CreateRenderTargetView(texture.Get(), &view, ctx.rtv());

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 total = 0;
        ctx.device->GetCopyableFootprints(&desc, 1, 1, 0, &footprint, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, total);

        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        list->ClearRenderTargetView(ctx.rtv(), blue, 0, nullptr);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), footprint), src(texture.Get(), 1);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
        check_pixel("mip 1", read_pixel(readback.Get(), footprint.Footprint.RowPitch, 5, 5), {0, 0, 255, 255});
    }

    {
        // Indexed draw through an index buffer view that starts inside its buffer.
        Scene scene;
        const uint32_t indices[] = {0, 0, 0, 3, 4, 5};
        ComPtr<ID3D12Resource> index_buffer = scene.ctx.create_upload_buffer(indices, sizeof(indices));
        const D3D12_INDEX_BUFFER_VIEW ibv = {index_buffer->GetGPUVirtualAddress() + 3 * sizeof(uint32_t),
                                             3 * sizeof(uint32_t), DXGI_FORMAT_R32_UINT};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.list->IASetIndexBuffer(&ibv);
        scene.list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
        scene.list->DrawIndexedInstanced(3, 1, 0, 0, 0);

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        CHECK(scene.run(&readback, &row_pitch) == 1);
        check_pixel("indexed", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
        check_pixel("not drawn", read_pixel(readback.Get(), row_pitch, 16, 32), {0, 0, 255, 255});
    }

    {
        // A draw that reads past the index buffer view is skipped.
        Scene scene;
        const uint32_t indices[] = {3, 4, 5};
        ComPtr<ID3D12Resource> index_buffer = scene.ctx.create_upload_buffer(indices, sizeof(indices));
        const D3D12_INDEX_BUFFER_VIEW ibv = {index_buffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R32_UINT};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.ctx.rtv();
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        scene.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        scene.list->IASetIndexBuffer(&ibv);
        scene.list->SetGraphicsRoot32BitConstants(0, 4, green, 0);
        scene.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
        scene.list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
        scene.list->DrawIndexedInstanced(3, 1, 1, 0, 0);  // indices 1..3: one past the end

        ComPtr<ID3D12Resource> readback;
        UINT row_pitch = 0;
        scene.run(&readback, &row_pitch);
        check_pixel("valid draw stays", read_pixel(readback.Get(), row_pitch, 48, 32), {0, 255, 0, 255});
    }
    return 0;
}
