// Shared setup for the rendering and copy tests: a device, a direct queue and a
// fence for waiting on the queue, plus small resource-creation helpers.
#pragma once

#include <cstdint>
#include <vector>

#include "dxgi/dxgi_interfaces.h"
#include "test_util.h"

struct TestContext {
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    std::vector<Com<ID3D12CommandAllocator>> allocators;  // one per list made by create_list

    TestContext()
    {
        Com<IDXGIFactory4> factory;
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));
        Com<IDXGIAdapter1> adapter;
        CHECK_HR(factory->EnumAdapters1(0, adapter.put()));
        CHECK_HR(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.put())));
        CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
    }

    // Blocks until everything submitted so far has finished on the GPU.
    void wait_idle()
    {
        CHECK_HR(queue->Signal(fence.get(), ++fence_value));
        CHECK_HR(fence->SetEventOnCompletion(fence_value, nullptr));
        CHECK(fence->GetCompletedValue() >= fence_value);
    }

    Com<ID3D12Resource> create_buffer(D3D12_HEAP_TYPE heap_type, UINT64 size)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = heap_type;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Com<ID3D12Resource> buffer;
        CHECK_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(buffer.put())));
        return buffer;
    }

    // A direct command list in the recording state, with an allocator of its own
    // (the last of `allocators`).
    Com<ID3D12GraphicsCommandList> create_list(ID3D12PipelineState *initial_state = nullptr)
    {
        allocators.emplace_back();
        CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocators.back().put())));
        Com<ID3D12GraphicsCommandList> list;
        CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators.back().get(), initial_state,
                                           IID_PPV_ARGS(list.put())));
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
