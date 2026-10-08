#include "d3d12/command_queue.h"

#include <vector>

#include "d3d12/command_list.h"
#include "d3d12/device.h"
#include "d3d12/fence.h"

namespace d3d12m {

HRESULT CommandQueue::create(Device *device, const D3D12_COMMAND_QUEUE_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE
        && desc.Type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_INVALIDARG;
    auto *queue = new CommandQueue(device);
    queue->desc_ = desc;
    if (mtlb_queue_create(device->handle(), &queue->queue_) != MTLB_OK) {
        D3D12M_LOG("queue creation failed: %s", mtlb_last_error());
        queue->Release();
        return E_FAIL;
    }
    HRESULT hr = queue->QueryInterface(riid, out);
    queue->Release();
    return hr;
}

CommandQueue::~CommandQueue()
{
    if (queue_)
        mtlb_queue_destroy(queue_);
}

void CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists)
{
    // All lists go to the backend in a single submit, one span per list.
    std::vector<mtlb_span> spans(count);
    for (UINT i = 0; i < count; ++i) {
        // Only this layer's command lists can be submitted.
        auto *list = static_cast<CommandList *>(static_cast<ID3D12GraphicsCommandList1 *>(lists[i]));
        if (!list->closed()) {
            D3D12M_LOG("ExecuteCommandLists: command list %u is still recording", i);
            return;
        }
        spans[i] = {list->stream().data(), list->stream().size()};
    }
    if (mtlb_queue_submit(queue_, spans.data(), count) != MTLB_OK)
        D3D12M_LOG("command submission failed: %s", mtlb_last_error());
}

HRESULT CommandQueue::Signal(ID3D12Fence *fence, UINT64 value)
{
    if (!fence)
        return E_INVALIDARG;
    return mtlb_queue_signal(queue_, static_cast<Fence *>(fence)->event(), value) == MTLB_OK ? S_OK : E_FAIL;
}

HRESULT CommandQueue::Wait(ID3D12Fence *fence, UINT64 value)
{
    if (!fence)
        return E_INVALIDARG;
    return mtlb_queue_wait(queue_, static_cast<Fence *>(fence)->event(), value) == MTLB_OK ? S_OK : E_FAIL;
}

HRESULT CommandQueue::GetTimestampFrequency(UINT64 *frequency)
{
    if (!frequency)
        return E_INVALIDARG;
    *frequency = 1000000000;  // Metal timestamps are in nanoseconds
    return S_OK;
}

} // namespace d3d12m
