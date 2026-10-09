// SPDX-License-Identifier: LGPL-2.1-or-later
// Shared setup for the rendering and copy tests: a device, a direct queue and a
// fence for waiting on the queue, plus small resource-creation helpers.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "dxgi/dxgi_interfaces.h"
#include "test_util.h"

#include <directx/d3dx12.h>

struct TestContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;  // one per list made by create_list

    TestContext()
    {
        ComPtr<IDXGIFactory4> factory;
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));
        ComPtr<IDXGIAdapter1> adapter;
        CHECK_HR(factory->EnumAdapters1(0, adapter.ReleaseAndGetAddressOf()));
        CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf())));
        CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())));
    }

    // Blocks until everything submitted so far has finished on the GPU.
    void wait_idle()
    {
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        CHECK_HR(fence->SetEventOnCompletion(fence_value, nullptr));
        CHECK(fence->GetCompletedValue() >= fence_value);
    }

    ComPtr<ID3D12Resource> create_buffer(D3D12_HEAP_TYPE heap_type, UINT64 size)
    {
        const CD3DX12_HEAP_PROPERTIES heap(heap_type);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
        ComPtr<ID3D12Resource> buffer;
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())));
        return buffer;
    }

    // Creates an upload buffer holding `data`.
    ComPtr<ID3D12Resource> create_upload_buffer(const void *data, UINT64 size)
    {
        ComPtr<ID3D12Resource> buffer = create_buffer(D3D12_HEAP_TYPE_UPLOAD, size);
        void *mapped = nullptr;
        CHECK_HR(buffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, size);
        buffer->Unmap(0, nullptr);
        return buffer;
    }

    // A direct command list in the recording state, with an allocator of its own
    // (the last of `allocators`).
    ComPtr<ID3D12GraphicsCommandList> create_list(ID3D12PipelineState *initial_state = nullptr)
    {
        allocators.emplace_back();
        CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(allocators.back().ReleaseAndGetAddressOf())));
        ComPtr<ID3D12GraphicsCommandList> list;
        CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators.back().Get(), initial_state,
                                           IID_PPV_ARGS(list.ReleaseAndGetAddressOf())));
        return list;
    }

    // Executes one closed command list and waits for it.
    void execute_and_wait(ID3D12GraphicsCommandList *list)
    {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        wait_idle();
    }
};
