// SPDX-License-Identifier: LGPL-2.1-or-later
// Several queues: direct, compute and copy queues each have a Metal queue of their own and are ordered
// against each other only by fences (shared events). Producers on one queue feed consumers on another; a wait
// can be submitted before the work that signals it.
#include "fill_cs.h"
#include "t12.h"

namespace {

struct Queues {
    Gpu &gpu;
    ComPtr<ID3D12CommandQueue> copy, compute;
    ComPtr<ID3D12Fence> copy_fence, compute_fence, direct_fence;
    UINT64 copy_value = 0, compute_value = 0, direct_value = 0;

    explicit Queues(Gpu &g) : gpu(g)
    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        CHECK_HR(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(copy.GetAddressOf())));
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        CHECK_HR(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(compute.GetAddressOf())));
        CHECK(copy->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY);
        CHECK(compute->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(copy_fence.GetAddressOf())));
        CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(compute_fence.GetAddressOf())));
        CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(direct_fence.GetAddressOf())));
    }

    ComPtr<ID3D12GraphicsCommandList> list(D3D12_COMMAND_LIST_TYPE type) { return gpu.list(type); }
};

void wait_cpu(ID3D12Fence *fence, UINT64 value)
{
    CHECK_HR(fence->SetEventOnCompletion(value, nullptr));
    CHECK(fence->GetCompletedValue() >= value);
}

} // namespace

int main()
{
    Gpu gpu;
    Queues q(gpu);

    // ---- copy queue -> direct queue --------------------------------------------------------------------------------
    for (int round = 0; round < 30; ++round) {
        std::vector<uint32_t> data(1024, 0x10000u * round + 7);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), 4096);
        ComPtr<ID3D12Resource> staging = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 4096);
        ComPtr<ID3D12Resource> result = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 4096);

        ComPtr<ID3D12GraphicsCommandList> produce = q.list(D3D12_COMMAND_LIST_TYPE_COPY);
        produce->CopyBufferRegion(staging.Get(), 0, upload.Get(), 0, 4096);
        CHECK_HR(produce->Close());
        ComPtr<ID3D12GraphicsCommandList> consume = gpu.list();
        consume->CopyBufferRegion(result.Get(), 0, staging.Get(), 0, 4096);
        CHECK_HR(consume->Close());

        const UINT64 value = ++q.copy_value;
        if (round % 2 == 0) {
            // The consumer's wait is submitted first.
            CHECK_HR(gpu.queue->Wait(q.copy_fence.Get(), value));
            gpu.execute(consume.Get());
            gpu.execute(produce.Get(), q.copy.Get());
            CHECK_HR(q.copy->Signal(q.copy_fence.Get(), value));
        } else {
            gpu.execute(produce.Get(), q.copy.Get());
            CHECK_HR(q.copy->Signal(q.copy_fence.Get(), value));
            CHECK_HR(gpu.queue->Wait(q.copy_fence.Get(), value));
            gpu.execute(consume.Get());
        }
        gpu.wait_idle();
        void *mapped = nullptr;
        CHECK_HR(result->Map(0, nullptr, &mapped));
        CHECK(std::memcmp(mapped, data.data(), 4096) == 0);
        result->Unmap(0, nullptr);
    }

    // ---- compute queue -> direct queue -------------------------------------------------------------------------------
    {
        const D3D12_DESCRIPTOR_RANGE1 range = descriptor_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0);
        const D3D12_ROOT_PARAMETER1 parameters[] = {root_constants(0, 4), descriptor_table(&range, 1)};
        ComPtr<ID3D12RootSignature> signature = gpu.root_signature(parameters, 2, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3D12PipelineState> pso = gpu.compute_pso(signature.Get(), T12_SHADER(g_fill_cs));
        ComPtr<ID3D12DescriptorHeap> heap = gpu.descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
        ComPtr<ID3D12Resource> buffer = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 16 * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> filler = gpu.texture(tex2d_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        for (UINT slot = 0; slot < 6; ++slot) {
            // The shader declares six UAVs; only the first two are used, the rest are null.
            D3D12_UNORDERED_ACCESS_VIEW_DESC null_desc = {};
            null_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            null_desc.Format = DXGI_FORMAT_R32_UINT;
            gpu.device->CreateUnorderedAccessView(nullptr, nullptr, &null_desc, gpu.cpu_handle(heap.Get(), slot));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpu.device->CreateUnorderedAccessView(filler.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 0));
        desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 256;
        desc.Buffer.StructureByteStride = 4;
        gpu.device->CreateUnorderedAccessView(buffer.Get(), nullptr, &desc, gpu.cpu_handle(heap.Get(), 1));

        for (int round = 0; round < 20; ++round) {
            ComPtr<ID3D12GraphicsCommandList> produce = q.list(D3D12_COMMAND_LIST_TYPE_COMPUTE);
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            produce->SetDescriptorHeaps(1, heaps);
            produce->SetComputeRootSignature(signature.Get());
            produce->SetPipelineState(pso.Get());
            produce->SetComputeRootDescriptorTable(1, gpu.gpu_handle(heap.Get(), 0));
            const UINT constants[4] = {16, 16, 1u, 0};  // mode 1: add `value` to what is there: starts at zero
            const UINT fill_constants[4] = {16, 16, 0u, 0};
            produce->SetComputeRoot32BitConstants(0, 4, fill_constants, 0);
            produce->Dispatch(2, 2, 1);
            const D3D12_RESOURCE_BARRIER barrier = uav_barrier(buffer.Get());
            produce->ResourceBarrier(1, &barrier);
            const UINT add[4] = {16, 16, constants[2], UINT(round)};
            produce->SetComputeRoot32BitConstants(0, 4, add, 0);
            produce->Dispatch(2, 2, 1);
            CHECK_HR(produce->Close());

            ComPtr<ID3D12Resource> result = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 1024);
            ComPtr<ID3D12GraphicsCommandList> consume = gpu.list();
            consume->CopyBufferRegion(result.Get(), 0, buffer.Get(), 0, 1024);
            CHECK_HR(consume->Close());

            const UINT64 value = ++q.compute_value;
            gpu.execute(produce.Get(), q.compute.Get());
            CHECK_HR(q.compute->Signal(q.compute_fence.Get(), value));
            CHECK_HR(gpu.queue->Wait(q.compute_fence.Get(), value));
            gpu.execute(consume.Get());
            gpu.wait_idle();
            uint32_t words[256];
            void *mapped = nullptr;
            CHECK_HR(result->Map(0, nullptr, &mapped));
            std::memcpy(words, mapped, sizeof(words));
            result->Unmap(0, nullptr);
            for (UINT i = 0; i < 256; ++i)
                CHECK_EQ(words[i], (0xA5000000u | ((i / 16) << 8) | (i % 16)) + UINT(round));
        }
    }

    // ---- direct queue -> copy queue, and fences that several queues wait for ----------------------------------------------
    {
        ComPtr<ID3D12Resource> source = gpu.buffer(D3D12_HEAP_TYPE_DEFAULT, 4096);
        ComPtr<ID3D12Resource> result = gpu.buffer(D3D12_HEAP_TYPE_READBACK, 4096);
        std::vector<uint32_t> data(1024, 0xC0FFEE);
        ComPtr<ID3D12Resource> upload = gpu.upload_buffer(data.data(), 4096);
        ComPtr<ID3D12GraphicsCommandList> produce = gpu.list();
        produce->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, 4096);
        CHECK_HR(produce->Close());
        ComPtr<ID3D12GraphicsCommandList> consume = q.list(D3D12_COMMAND_LIST_TYPE_COPY);
        consume->CopyBufferRegion(result.Get(), 0, source.Get(), 0, 4096);
        CHECK_HR(consume->Close());
        const UINT64 value = ++q.direct_value;
        CHECK_HR(q.copy->Wait(q.direct_fence.Get(), value));
        CHECK_HR(q.compute->Wait(q.direct_fence.Get(), value));  // a second waiter on the same value
        gpu.execute(consume.Get(), q.copy.Get());
        gpu.execute(produce.Get());
        CHECK_HR(gpu.queue->Signal(q.direct_fence.Get(), value));
        CHECK_HR(q.copy->Signal(q.copy_fence.Get(), ++q.copy_value));
        wait_cpu(q.copy_fence.Get(), q.copy_value);
        void *mapped = nullptr;
        CHECK_HR(result->Map(0, nullptr, &mapped));
        CHECK(std::memcmp(mapped, data.data(), 4096) == 0);
        result->Unmap(0, nullptr);
        CHECK(q.direct_fence->GetCompletedValue() >= value);
    }

    UINT64 frequency = 0;
    CHECK_HR(q.copy->GetTimestampFrequency(&frequency));
    CHECK(frequency > 0);
    gpu.wait_idle();
    std::printf("p_queues: PASS\n");
    return 0;
}
