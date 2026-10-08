// ID3D12Fence::SetEventOnCompletion with a real event handle.
#include "common/platform.h"
#include "test_context.h"

int main()
{
    TestContext ctx;
    HANDLE event = d3d12metal_native_create_event();

    // Already reached: the event is signaled before the call returns.
    CHECK_HR(ctx.fence->Signal(1));
    CHECK_HR(ctx.fence->SetEventOnCompletion(1, event));
    CHECK(d3d12metal_native_wait_event(event, 0));

    // Not reached yet: stays unsignaled until the queue signals the fence.
    CHECK_HR(ctx.fence->SetEventOnCompletion(10, event));
    CHECK(!d3d12metal_native_wait_event(event, 50));
    CHECK_HR(ctx.queue->Signal(ctx.fence.get(), 10));
    CHECK(d3d12metal_native_wait_event(event, 5000));
    CHECK(ctx.fence->GetCompletedValue() >= 10);

    // Several pending waits are released independently, in value order.
    HANDLE second = d3d12metal_native_create_event();
    CHECK_HR(ctx.fence->SetEventOnCompletion(30, event));
    CHECK_HR(ctx.fence->SetEventOnCompletion(20, second));
    CHECK_HR(ctx.fence->Signal(25));
    CHECK(d3d12metal_native_wait_event(second, 5000));
    CHECK(!d3d12metal_native_wait_event(event, 50));
    CHECK_HR(ctx.fence->Signal(30));
    CHECK(d3d12metal_native_wait_event(event, 5000));

    // Releasing a fence with a pending wait must not hang or crash.
    Com<ID3D12Fence> pending;
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(pending.put())));
    CHECK_HR(pending->SetEventOnCompletion(100, event));
    pending.reset();

    d3d12metal_native_destroy_event(second);
    d3d12metal_native_destroy_event(event);
    return 0;
}
