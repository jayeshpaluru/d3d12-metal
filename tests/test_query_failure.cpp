// SPDX-License-Identifier: LGPL-2.1-or-later
// A timestamp heap whose counter sample buffer cannot be made: the failure is remembered, creation is not tried again
// for every timestamp, and the queue keeps working.
#include "bridge/mtlb.h"
#include "d3d12/query_heap.h"
#include "test_context.h"

int main()
{
    TestContext ctx;
    D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
    ComPtr<ID3D12QueryHeap> heap;
    CHECK_HR(ctx.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())));
    const mtlb_query_heap handle = static_cast<d3d12m::QueryHeap *>(heap.Get())->handle();
    ComPtr<ID3D12Resource> results = ctx.create_buffer(D3D12_HEAP_TYPE_READBACK, 4 * 8);

    mtlb_query_heap_test_sample_attempts(handle, 1);  // creation fails from now on
    for (int round = 0; round < 10; ++round) {
        ComPtr<ID3D12GraphicsCommandList> list = ctx.create_list();
        for (UINT i = 0; i < 4; ++i)
            list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
        list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, results.Get(), 0);
        CHECK_HR(list->Close());
        ctx.execute_and_wait(list.Get());
    }
    // One try for the one buffer of this heap, not one per timestamp.
    CHECK(mtlb_query_heap_test_sample_attempts(handle, -1) == 1);
    return 0;
}
