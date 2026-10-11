// SPDX-License-Identifier: LGPL-2.1-or-later
// Asynchronous submission: a queue's worker replays command lists, signals and waits in API order after the calls
// returned. Run twice, with the default and with D3D12METAL_ASYNC_SUBMIT=0: the results must be the same.
#include <chrono>
#include <cstdlib>
#include <thread>

#include "d3d12/command_queue.h"
#include "render_context.h"

namespace {

constexpr int kRounds = 100;

// Records "clear the target to `color`, copy it to a new readback buffer" into `list` (reset and left closed).
ComPtr<ID3D12Resource> record_clear(RenderContext &ctx, ID3D12GraphicsCommandList *list, const float color[4])
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.rtv();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    UINT row_pitch = 0;
    ComPtr<ID3D12Resource> readback = ctx.record_readback(list, &row_pitch);
    const CD3DX12_RESOURCE_BARRIER back = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &back);
    CHECK_HR(list->Close());
    return readback;
}

UINT pitch_of(RenderContext &ctx)
{
    const D3D12_RESOURCE_DESC desc = ctx.target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    ctx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    return footprint.Footprint.RowPitch;
}

} // namespace

int main()
{
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
    RenderContext ctx;
    auto *queue = static_cast<d3d12m::CommandQueue *>(ctx.queue.Get());
    const UINT pitch = pitch_of(ctx);

    {
        // Two threads submit lists and signals on one queue; the last list of all decides the target's colour.
        ComPtr<ID3D12GraphicsCommandList> lists[2] = {ctx.create_list(), ctx.create_list()};
        ComPtr<ID3D12Resource> unused[2] = {record_clear(ctx, lists[0].Get(), red), record_clear(ctx, lists[1].Get(), yellow)};
        ComPtr<ID3D12Fence> fences[2];
        for (auto &fence : fences)
            CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())));
        std::thread threads[2];
        for (int t = 0; t < 2; ++t) {
            threads[t] = std::thread([&, t] {
                ID3D12CommandList *list = lists[t].Get();
                for (int i = 1; i <= kRounds; ++i) {
                    ctx.queue->ExecuteCommandLists(1, &list);
                    CHECK_HR(ctx.queue->Signal(fences[t].Get(), i));
                }
            });
        }
        for (auto &thread : threads)
            thread.join();
        for (auto &fence : fences) {
            CHECK_HR(fence->SetEventOnCompletion(kRounds, nullptr));
            CHECK(fence->GetCompletedValue() == kRounds);
        }

        ComPtr<ID3D12GraphicsCommandList> last = ctx.create_list();
        ComPtr<ID3D12Resource> readback = record_clear(ctx, last.Get(), green);
        ctx.execute_and_wait(last.Get());
        check_pixel("last list wins", read_pixel(readback.Get(), pitch, 5, 5), {0, 255, 0, 255});
        // 2 * kRounds lists of one pass each, then the last one.
        CHECK(mtlb_queue_render_pass_count(queue->handle()) >= 2 * kRounds + 1);
    }

    {
        // Reset and re-record right after ExecuteCommandLists, with earlier work still queued: the first
        // submission must replay the first recording.
        ComPtr<ID3D12GraphicsCommandList> busy = ctx.create_list();
        record_clear(ctx, busy.Get(), blue);
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        for (int round = 0; round < 20; ++round) {
            ID3D12CommandList *busy_list = busy.Get();
            for (int i = 0; i < 20; ++i)
                ctx.queue->ExecuteCommandLists(1, &busy_list);
            if (round)
                CHECK_HR(list->Reset(ctx.allocators.back().Get(), nullptr));
            ComPtr<ID3D12Resource> first = record_clear(ctx, list.Get(), red);
            ID3D12CommandList *raw = list.Get();
            ctx.queue->ExecuteCommandLists(1, &raw);
            CHECK_HR(list->Reset(ctx.allocators.back().Get(), nullptr));
            ComPtr<ID3D12Resource> second = record_clear(ctx, list.Get(), green);
            ctx.queue->ExecuteCommandLists(1, &raw);
            ctx.wait_idle();
            check_pixel("first recording", read_pixel(first.Get(), pitch, 5, 5), {255, 0, 0, 255});
            check_pixel("second recording", read_pixel(second.Get(), pitch, 5, 5), {0, 255, 0, 255});
        }
    }

    {
        // The same closed list twice in one call and again in another: four replays, one pass each.
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        ComPtr<ID3D12Resource> readback = record_clear(ctx, list.Get(), yellow);
        ctx.wait_idle();
        const uint64_t before = mtlb_queue_render_pass_count(queue->handle());
        ID3D12CommandList *twice[] = {list.Get(), list.Get()};
        ctx.queue->ExecuteCommandLists(2, twice);
        ctx.queue->ExecuteCommandLists(1, twice);
        ctx.queue->ExecuteCommandLists(1, twice);
        ctx.wait_idle();
        CHECK(mtlb_queue_render_pass_count(queue->handle()) - before == 4);
        check_pixel("executed again", read_pixel(readback.Get(), pitch, 5, 5), {255, 255, 0, 255});
    }

    {
        // A queue wait on a fence signalled later (by the CPU) holds back what follows, and markers pass through.
        ComPtr<ID3D12Fence> gate;
        CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate.ReleaseAndGetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        ComPtr<ID3D12Resource> readback = record_clear(ctx, list.Get(), red);
        CHECK_HR(ctx.queue->Wait(gate.Get(), 1));
        ctx.queue->BeginEvent(0, "async", 6);
        ID3D12CommandList *raw = list.Get();
        ctx.queue->ExecuteCommandLists(1, &raw);
        ctx.queue->EndEvent();
        ctx.queue->SetMarker(0, "mark", 5);
        CHECK_HR(ctx.queue->Signal(ctx.fence.Get(), ++ctx.fence_value));
        const UINT64 value = ctx.fence_value;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(ctx.fence->GetCompletedValue() < value);
        CHECK_HR(gate->Signal(1));
        CHECK_HR(ctx.fence->SetEventOnCompletion(value, nullptr));
        check_pixel("after the wait", read_pixel(readback.Get(), pitch, 5, 5), {255, 0, 0, 255});
    }

    {
        // Deferred commits: the worker puts an Execute+Signal pair into one command buffer, and never leaves work
        // uncommitted when it goes idle.
        const char *async_env = getenv("D3D12METAL_ASYNC_SUBMIT");
        const bool async = !async_env || strcmp(async_env, "0") != 0;
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        ComPtr<ID3D12Resource> readback = record_clear(ctx, list.Get(), blue);
        ID3D12CommandList *raw = list.Get();
        ComPtr<ID3D12Fence> fence;
        CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())));
        ctx.wait_idle();
        constexpr int kPairs = 200;
        mtlb_stats before, after;
        mtlb_stats_get(&before);
        for (int i = 1; i <= kPairs; ++i) {
            ctx.queue->ExecuteCommandLists(1, &raw);
            CHECK_HR(ctx.queue->Signal(fence.Get(), i));
        }
        queue->flush();
        mtlb_stats_get(&after);
        const double per_pair = double(after.command_buffers - before.command_buffers) / kPairs;
        printf("command buffers per Execute+Signal pair: %.2f\n", per_pair);
        if (async)
            CHECK(per_pair < 1.5);
        // (c) a CPU wait on a value signalled right after Execute returns.
        CHECK_HR(fence->SetEventOnCompletion(kPairs, nullptr));
        CHECK(fence->GetCompletedValue() == kPairs);

        // (b) a lone Execute with no Signal still runs: the later signal's wait sees its result.
        ComPtr<ID3D12GraphicsCommandList> lone = ctx.create_list();
        ComPtr<ID3D12Resource> lone_readback = record_clear(ctx, lone.Get(), red);
        ID3D12CommandList *lone_raw = lone.Get();
        ctx.queue->ExecuteCommandLists(1, &lone_raw);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_HR(ctx.queue->Signal(fence.Get(), kPairs + 1));
        CHECK_HR(fence->SetEventOnCompletion(kPairs + 1, nullptr));
        check_pixel("lone execute", read_pixel(lone_readback.Get(), pitch, 5, 5), {255, 0, 0, 255});

        // (d) the completed value arrives with no further API call on the queue.
        ctx.queue->ExecuteCommandLists(1, &raw);
        CHECK_HR(ctx.queue->Signal(fence.Get(), kPairs + 2));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fence->GetCompletedValue() < kPairs + 2 && std::chrono::steady_clock::now() < deadline) {
        }
        CHECK(fence->GetCompletedValue() == kPairs + 2);
    }

    {
        // Lists released while queued are replayed first; the queue then shuts down with work in flight.
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        record_clear(ctx, list.Get(), blue);
        ID3D12CommandList *raw = list.Get();
        for (int i = 0; i < 50; ++i)
            ctx.queue->ExecuteCommandLists(1, &raw);
        list.Reset();
    }
    return 0;
}
