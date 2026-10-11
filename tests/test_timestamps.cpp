// SPDX-License-Identifier: LGPL-2.1-or-later
// Timestamps are sampled at the start of the next encoder, so they do not split render passes; consecutive ones
// share a sample (equal values), and results stay ordered across sample buffers and command lists.
#include "d3d12/command_queue.h"
#include "render_context.h"

namespace {

constexpr UINT64 kBigSize = 64 * 1024 * 1024;

struct Timestamps {
    RenderContext ctx;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> big = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, kBigSize);
    ComPtr<ID3D12Resource> big2 = ctx.create_buffer(D3D12_HEAP_TYPE_DEFAULT, kBigSize);

    Timestamps()
    {
        D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8192, 0};
        CHECK_HR(ctx.device->CreateQueryHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
    }

    void stamp(ID3D12GraphicsCommandList *list, UINT index) { list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index); }

    // Some GPU time between timestamps.
    void work(ID3D12GraphicsCommandList *list)
    {
        for (int i = 0; i < 4; ++i) {
            list->CopyBufferRegion(big2.Get(), 0, big.Get(), 0, kBigSize);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            list->ResourceBarrier(1, &barrier);
        }
    }

    void resolve(ID3D12GraphicsCommandList *list, UINT start, UINT count, ID3D12Resource *dst)
    {
        list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, start, count, dst, 0);
    }

    std::vector<uint64_t> read(ID3D12Resource *buffer, UINT count)
    {
        std::vector<uint64_t> values(count);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(values.data(), mapped, count * 8);
        buffer->Unmap(0, nullptr);
        return values;
    }
};

} // namespace

int main()
{
    Timestamps t;
    RenderContext &ctx = t.ctx;

    {
        // Timestamps between the draws of a pass leave it whole.
        CD3DX12_ROOT_PARAMETER1 param;
        param.InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        ComPtr<ID3D12RootSignature> signature = ctx.create_root_signature(&param, 1);
        ComPtr<ID3D12PipelineState> pso = ctx.create_color_pso(signature.Get());
        const float vertices[] = {-0.9f, -0.9f, 0.0f, -0.5f, 0.9f, 0.0f, -0.1f, -0.9f, 0.0f};
        ComPtr<ID3D12Resource> vertex_buffer = ctx.create_upload_buffer(vertices, sizeof(vertices));
        const D3D12_VERTEX_BUFFER_VIEW vbv = {vertex_buffer->GetGPUVirtualAddress(), sizeof(vertices), 3 * sizeof(float)};
        const float red[4] = {1, 0, 0, 1};

        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list(pso.Get());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.rtv();
        list->SetGraphicsRootSignature(signature.Get());
        ctx.set_viewport_and_scissor(list.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
        t.stamp(list.Get(), 0);
        list->DrawInstanced(3, 1, 0, 0);
        t.stamp(list.Get(), 1);
        list->DrawInstanced(3, 1, 0, 0);
        t.stamp(list.Get(), 2);
        list->DrawInstanced(3, 1, 0, 0);
        t.stamp(list.Get(), 3);
        UINT row_pitch = 0;
        ComPtr<ID3D12Resource> readback = ctx.record_readback(list.Get(), &row_pitch);
        CHECK_HR(list->Close());
        auto *queue = static_cast<d3d12m::CommandQueue *>(ctx.queue.Get());
        const uint64_t before = mtlb_queue_render_pass_count(queue->handle());
        ctx.execute_and_wait(list.Get());
        CHECK(mtlb_queue_render_pass_count(queue->handle()) - before == 1);
        check_pixel("drawn", read_pixel(readback.Get(), row_pitch, 16, 32), {255, 0, 0, 255});
    }

    {
        // Ordered values around work, consecutive timestamps sharing a sample, and two sample buffers (4096 each).
        ComPtr<ID3D12Resource> results = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 8 * 8);
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        t.stamp(list.Get(), 4094);
        t.work(list.Get());
        t.stamp(list.Get(), 4095);
        t.stamp(list.Get(), 4096);
        t.stamp(list.Get(), 4097);
        t.work(list.Get());
        t.stamp(list.Get(), 4098);
        t.stamp(list.Get(), 4099);
        t.resolve(list.Get(), 4094, 6, results.Get());
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
        const std::vector<uint64_t> v = t.read(results.Get(), 6);
        for (uint64_t value : v)
            CHECK(value != 0);
        for (size_t i = 1; i < v.size(); ++i)
            CHECK(v[i] >= v[i - 1]);
        CHECK(v[5] > v[0]);
        CHECK(v[3] > v[0]);
    }

    {
        // A timestamp at the end of a list: read by a later list, and by the same one.
        ComPtr<ID3D12Resource> results = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 2 * 8);
        ComPtr<ID3D12GraphicsCommandList> first = ctx.create_list();
        t.work(first.Get());
        t.stamp(first.Get(), 10);
        t.work(first.Get());
        t.stamp(first.Get(), 11);
        t.resolve(first.Get(), 11, 1, results.Get());
        CHECK_HR(first->Close());
        ctx.execute_and_wait(first.Get());
        const uint64_t same = t.read(results.Get(), 1)[0];
        CHECK(same != 0);

        ComPtr<ID3D12GraphicsCommandList> second = ctx.create_list();
        t.resolve(second.Get(), 10, 2, results.Get());
        CHECK_HR(second->Close());
        ctx.execute_and_wait(second.Get());
        const std::vector<uint64_t> v = t.read(results.Get(), 2);
        CHECK(v[0] != 0);
        CHECK(v[1] == same);
        CHECK(v[1] > v[0]);
    }
    return 0;
}
