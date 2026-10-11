// SPDX-License-Identifier: LGPL-2.1-or-later
// The fence mirror (what GetCompletedValue reads) follows the event's actual value (Metal's events never go down), and a signal whose command buffer failed still advances it, so that a spin on
// GetCompletedValue after a GPU error ends.
#include <chrono>

#include "bridge/mtlb.h"
#include "common/platform.h"
#include "d3d12/command_queue.h"
#include "d3d12/fence.h"
#include "test_context.h"

namespace {

// Spins on GetCompletedValue until it satisfies `done`, for at most five seconds.
template <class Done>
UINT64 spin(ID3D12Fence *fence, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    UINT64 value;
    while (!done(value = fence->GetCompletedValue()) && std::chrono::steady_clock::now() < deadline) {
    }
    return value;
}

}  // namespace

int main()
{
    TestContext ctx;
    auto *queue = static_cast<d3d12m::CommandQueue *>(ctx.queue.Get());

    // A lower value: Metal's event stays where it is, and so does the mirror (it must not claim a value that the
    // event, and the waits on it, do not have).
    ComPtr<ID3D12Fence> fence;
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
    auto *ours = static_cast<d3d12m::Fence *>(fence.Get());
    CHECK_HR(ctx.queue->Signal(fence.Get(), 10));
    CHECK(spin(fence.Get(), [](UINT64 v) { return v == 10; }) == 10);
    CHECK_HR(ctx.queue->Signal(fence.Get(), 5));
    ComPtr<ID3D12Fence> after;  // completes after the signal above
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(after.GetAddressOf())));
    CHECK_HR(ctx.queue->Signal(after.Get(), 1));
    CHECK(spin(after.Get(), [](UINT64 v) { return v == 1; }) == 1);
    CHECK(fence->GetCompletedValue() == mtlb_event_completed_value(ours->event()));
    CHECK_HR(fence->Signal(3));  // from the CPU
    CHECK(fence->GetCompletedValue() == mtlb_event_completed_value(ours->event()));
    CHECK(fence->GetCompletedValue() >= 10);
    CHECK_HR(ctx.queue->Signal(fence.Get(), 11));
    CHECK(spin(fence.Get(), [](UINT64 v) { return v == 11; }) == 11);

    // A signal that never executed (its command buffer failed): the fence reaches the value anyway.
    CHECK(mtlb_queue_test_drop_signals(queue->handle(), 1) == MTLB_OK);
    CHECK_HR(ctx.queue->Signal(fence.Get(), 20));
    CHECK(spin(fence.Get(), [](UINT64 v) { return v >= 20; }) == 20);
    CHECK(mtlb_event_completed_value(ours->event()) == 20);
    HANDLE event = d3d12metal_native_create_event();
    CHECK_HR(fence->SetEventOnCompletion(20, event));
    CHECK(d3d12metal_native_wait_event(event, 5000));

    // Signals after it behave as before.
    CHECK_HR(ctx.queue->Signal(fence.Get(), 21));
    CHECK(spin(fence.Get(), [](UINT64 v) { return v == 21; }) == 21);
    d3d12metal_native_destroy_event(event);
    std::printf("test_fence_mirror: PASS\n");
    return 0;
}
