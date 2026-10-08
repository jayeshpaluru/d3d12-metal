// Queue-side Signal and Wait, batched with submits: a signal needs no submit to
// flush, a signal after ExecuteCommandLists covers the work before it, and a
// queue wait holds back later signals until the fence is reached.
#include <cstring>

#include "common/platform.h"
#include "test_context.h"

int main()
{
    TestContext ctx;

    // Signal with nothing submitted.
    CHECK_HR(ctx.queue->Signal(ctx.fence.Get(), 1));
    CHECK_HR(ctx.fence->SetEventOnCompletion(1, nullptr));

    // Execute then Signal: the copy is complete once the fence is.
    ComPtr<ID3D12Resource> upload = ctx.create_buffer(D3D12_HEAP_TYPE_UPLOAD, 256);
    ComPtr<ID3D12Resource> readback = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 256);
    void *mapped = nullptr;
    CHECK_HR(upload->Map(0, nullptr, &mapped));
    std::memset(mapped, 0x5a, 256);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
    list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 256);
    CHECK_HR(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    ctx.queue->ExecuteCommandLists(1, lists);
    CHECK_HR(ctx.queue->Signal(ctx.fence.Get(), 2));
    CHECK_HR(ctx.fence->SetEventOnCompletion(2, nullptr));
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    CHECK(static_cast<uint8_t *>(mapped)[255] == 0x5a);
    readback->Unmap(0, nullptr);

    // Wait holds back everything after it, including a later Signal.
    ComPtr<ID3D12Fence> gate, done;
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate.ReleaseAndGetAddressOf())));
    CHECK_HR(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done.ReleaseAndGetAddressOf())));
    CHECK_HR(ctx.queue->Wait(gate.Get(), 5));
    CHECK_HR(ctx.queue->Signal(done.Get(), 1));
    HANDLE event = d3d12metal_native_create_event();
    CHECK_HR(done->SetEventOnCompletion(1, event));
    CHECK(!d3d12metal_native_wait_event(event, 100));
    CHECK(done->GetCompletedValue() == 0);
    CHECK_HR(gate->Signal(5));
    CHECK(d3d12metal_native_wait_event(event, 5000));
    d3d12metal_native_destroy_event(event);
    return 0;
}
