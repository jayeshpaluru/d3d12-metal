#include "d3d12/command_queue.h"

#include <new>
#include <vector>

#include "d3d12/command_list.h"
#include "d3d12/device.h"
#include "d3d12/fence.h"
#include "d3d12/resource.h"

namespace d3d12m {

HRESULT CommandQueue::create(Device *device, const D3D12_COMMAND_QUEUE_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!supported_list_type(desc.Type))
        return E_INVALIDARG;
    auto *queue = new CommandQueue(device);
    queue->desc_ = desc;
    mtlb_result result = mtlb_queue_create(device->handle(), &queue->queue_);
    if (result != MTLB_OK) {
        D3D12M_LOG("queue creation failed: %s", mtlb_last_error());
        queue->Release();
        return to_hresult(result);
    }
    return hand_out(queue, riid, out);
}

CommandQueue::~CommandQueue()
{
    if (queue_)
        mtlb_queue_destroy(queue_);
}

void CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists)
{
    // All lists go to the backend in a single submit, one span per list.
    // Placed render targets and depth-stencils created since the last submission are cleared first.
    std::vector<uint8_t> init_stream;
    std::vector<Resource *> initialized = device()->take_pending_init(init_stream);
    const UINT first = init_stream.empty() ? 0 : 1;
    std::vector<mtlb_span> spans;
    try {
        spans.resize(count + first);
    } catch (const std::bad_alloc &) {
        for (Resource *resource : initialized)
            resource->Release();
        return;
    }
    if (first)
        spans[0] = {init_stream.data(), init_stream.size()};
    for (UINT i = 0; i < count; ++i) {
        // Only this layer's command lists can be submitted.
        auto *list = ours<CommandList>(lists[i]);
        if (!list || !list->closed()) {
            D3D12M_LOG("ExecuteCommandLists: command list %u is %s", i, list ? "still recording" : "not from this layer");
            for (Resource *resource : initialized)
                resource->Release();
            return;
        }
        spans[first + i] = {list->stream().data(), list->stream().size()};
    }
    if (mtlb_queue_submit(queue_, spans.data(), count + first) != MTLB_OK)
        D3D12M_LOG("command submission failed: %s", mtlb_last_error());
    for (Resource *resource : initialized)
        resource->Release();
}

HRESULT CommandQueue::Signal(ID3D12Fence *fence, UINT64 value)
{
    auto *f = ours<Fence>(fence);
    if (!f)
        return E_INVALIDARG;
    return to_hresult(mtlb_queue_signal(queue_, f->event(), value));
}

HRESULT CommandQueue::Wait(ID3D12Fence *fence, UINT64 value)
{
    auto *f = ours<Fence>(fence);
    if (!f)
        return E_INVALIDARG;
    return to_hresult(mtlb_queue_wait(queue_, f->event(), value));
}

HRESULT CommandQueue::GetTimestampFrequency(UINT64 *frequency)
{
    if (!frequency)
        return E_INVALIDARG;
    *frequency = 1000000000;  // Metal timestamps are in nanoseconds
    return S_OK;
}

} // namespace d3d12m
