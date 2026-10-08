#include "d3d12/command_queue.h"

#include <new>
#include <vector>

#include "common/platform.h"
#include "d3d12/command_list.h"
#include "d3d12/device.h"
#include "d3d12/fence.h"
#include "d3d12/resource.h"

namespace d3d12m {

HRESULT CommandQueue::create(Device *device, const D3D12_COMMAND_QUEUE_DESC &desc, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!supported_queue_type(desc.Type))
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
    if (count && !lists)
        return;
    // All lists go to the backend in a single submit, one span per list.
    // Placed render targets and depth-stencils created since the last submission are cleared first.
    // The lists are validated first, so a rejected call leaves the clears pending for the next one.
    std::vector<mtlb_span> spans;
    try {
        spans.resize(count + 1);  // the first is for the clears
    } catch (const std::bad_alloc &) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        // Only this layer's command lists can be submitted.
        auto *list = ours<CommandList>(lists[i]);
        if (!list || !list->closed()) {
            D3D12M_LOG("ExecuteCommandLists: command list %u is %s", i, list ? "still recording" : "not from this layer");
            return;
        }
        spans[1 + i] = {list->stream().data(), list->stream().size()};
    }
    std::vector<uint8_t> init_stream;
    std::vector<Resource *> initialized = device()->take_pending_init(init_stream);
    if (!init_stream.empty())
        spans[0] = {init_stream.data(), init_stream.size()};
    const UINT first = init_stream.empty() ? 1 : 0;
    if (mtlb_queue_submit(queue_, spans.data() + first, count + 1 - first) != MTLB_OK)
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
    *frequency = mtlb_timestamp_frequency(device()->handle());
    return S_OK;
}

// The GPU clock in timestamp query ticks, and the CPU clock in QueryPerformanceCounter ticks, sampled together.
HRESULT CommandQueue::GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp)
{
    if (!gpu_timestamp || !cpu_timestamp)
        return E_INVALIDARG;
    uint64_t gpu, cpu_mach;
    if (mtlb_gpu_clock(device()->handle(), &gpu, &cpu_mach) != MTLB_OK)
        return E_FAIL;
    *gpu_timestamp = gpu;
    *cpu_timestamp = platform_performance_counter();
    return S_OK;
}

// Queue markers label the work the queue has recorded so far.
void CommandQueue::SetMarker(UINT metadata, const void *data, UINT size)
{
    const std::string text = marker_text(metadata, data, size);
    mtlb_queue_marker(queue_, 0, text.c_str());
    mtlb_queue_marker(queue_, 1, nullptr);
}

void CommandQueue::BeginEvent(UINT metadata, const void *data, UINT size)
{
    mtlb_queue_marker(queue_, 0, marker_text(metadata, data, size).c_str());
}

void CommandQueue::EndEvent()
{
    mtlb_queue_marker(queue_, 1, nullptr);
}

} // namespace d3d12m
