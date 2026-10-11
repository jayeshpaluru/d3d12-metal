// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12/command_queue.h"
#include "common/config.h"
#include "common/stats.h"

#include <new>
#include <string>
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
    flush();
    if (notify_) {
        mtlb_notify_close(notify_);
        worker_.join();
        mtlb_notify_destroy(notify_);
    }
    if (queue_)
        mtlb_queue_destroy(queue_);
}

// One queued operation. It owns the references it takes (released once executed) and is deleted after it ran.
struct CommandQueue::Op {
    enum class Kind { Submit, Signal, Wait, Marker, Present } kind;
    // Submit
    std::vector<CommandList *> lists;
    std::vector<uint8_t> init_stream;
    // The streams to replay, captured at enqueue: a Reset of a list meanwhile retires its stream instead of freeing it.
    std::vector<mtlb_span> spans;
    std::vector<Resource *> initialized;
    // Signal, Wait
    Fence *fence = nullptr;
    // Signal, Wait: the fence value; Present: the sync interval
    uint64_t value = 0;
    // Marker (0 push, 1 pop, 2 signpost)
    uint32_t marker_kind = 0;
    std::string text;
    // Present
    mtlb_swapchain swapchain = 0;
    Resource *buffer = nullptr;
    mtlb_texture texture = 0;
    // Runs after the bridge call, on the executing thread.
    std::function<void(mtlb_result)> done;

    explicit Op(Kind k) : kind(k) {}
};

void CommandQueue::dispatch(std::unique_ptr<Op> op)
{
    std::call_once(worker_started_, [this] {
        static const bool enabled = !config_get("ASYNC_SUBMIT") || config_flag("ASYNC_SUBMIT");
        if (enabled && mtlb_notify_create(&notify_) == MTLB_OK) {
            mtlb_queue_set_manual_commit(queue_, 1);
            try {
                worker_ = std::thread([this] { run(); });
                async_ = true;
            } catch (...) {
                D3D12M_LOG("cannot start the submit worker: operations run on the calling thread");
                mtlb_queue_set_manual_commit(queue_, 0);
                mtlb_notify_destroy(notify_);
                notify_ = 0;
            }
        }
    });
    if (!async_) {
        execute(*op);
        return;
    }
    posted_.fetch_add(1);
    mtlb_notify_post(notify_, reinterpret_cast<uint64_t>(op.release()), 0);
}

// The worker commits the queue's command buffer itself, so that consecutive operations share one: after a run of
// signals (so the fence mirror's completion handler is not delayed by later work) and before it blocks. flush()
// commits what the worker has not yet.
void CommandQueue::run()
{
    mtlb_notification batch[16];
    uint32_t count = 0;
    bool signal_ended = false;  // the last operation was a signal and no commit followed
    unsigned submits = 0;       // submits in the open buffer: a long run without signals still reaches the GPU
    const auto commit = [&] {
        mtlb_queue_commit(queue_);
        signal_ended = false;
        submits = 0;
    };
    while (mtlb_notify_wait(notify_, batch, 16, &count) == MTLB_OK && count) {
        for (uint32_t i = 0; i < count; ++i) {
            std::unique_ptr<Op> op(reinterpret_cast<Op *>(batch[i].cookie));
            if (signal_ended && op->kind != Op::Kind::Signal)
                commit();
            const Op::Kind kind = op->kind;
            try {
                execute(*op);
            } catch (...) {
                // (e.g. bad_alloc) the operation is lost, but the worker, and the game, go on.
                D3D12M_LOG("exception while executing a queued operation (dropped)");
            }
            op.reset();
            signal_ended = kind == Op::Kind::Signal;
            submits = kind == Op::Kind::Present ? 0 : submits + (kind == Op::Kind::Submit);  // a present commits
            if (submits >= 4 || (i + 1 == count && !mtlb_notify_pending(notify_)))
                commit();
            executed_.fetch_add(1);
            if (waiters_.load()) {
                std::lock_guard<std::mutex> lock(progress_mutex_);
                progress_.notify_all();
            }
        }
    }
}

void CommandQueue::flush()
{
    const uint64_t target = posted_.load();
    if (executed_.load() < target) {
        std::unique_lock<std::mutex> lock(progress_mutex_);
        waiters_.fetch_add(1);
        progress_.wait(lock, [&] { return executed_.load() >= target; });
        waiters_.fetch_sub(1);
    }
    // The worker may have left the last operations in an open buffer (it commits when it goes idle, or after a
    // few submits); committing here keeps their order and makes them reach the GPU.
    if (async_)
        mtlb_queue_commit(queue_);
}

void CommandQueue::execute(Op &op)
{
    mtlb_result result = MTLB_OK;
    switch (op.kind) {
    case Op::Kind::Submit: {
        {
            StatTimer timer(Stat::SubmitNanos);
            result = mtlb_queue_submit(queue_, op.spans.data(), static_cast<uint32_t>(op.spans.size()));
        }
        if (result != MTLB_OK)
            D3D12M_LOG("command submission failed: %s", mtlb_last_error());
        for (CommandList *list : op.lists) {
            list->end_submit();
            list->Release();
        }
        for (Resource *resource : op.initialized)
            resource->release_internal_ref();
        break;
    }
    case Op::Kind::Signal:
    case Op::Kind::Wait: {
        const bool signal = op.kind == Op::Kind::Signal;
        StatTimer timer(Stat::SignalNanos);
        result = signal ? mtlb_queue_signal(queue_, op.fence->event(), op.value)
                        : mtlb_queue_wait(queue_, op.fence->event(), op.value);
        if (result != MTLB_OK)
            D3D12M_LOG("queue %s failed: %s", signal ? "signal" : "wait", mtlb_last_error());
        break;
    }
    case Op::Kind::Marker:
        if (op.marker_kind == 2) {
            mtlb_queue_marker(queue_, 0, op.text.c_str());
            mtlb_queue_marker(queue_, 1, nullptr);
        } else {
            mtlb_queue_marker(queue_, op.marker_kind, op.marker_kind == 0 ? op.text.c_str() : nullptr);
        }
        break;
    case Op::Kind::Present: {
        {
            StatTimer timer(Stat::PresentNanos);
            result = mtlb_queue_present(queue_, op.swapchain, op.texture, static_cast<uint32_t>(op.value));
        }
        if (result != MTLB_OK)
            D3D12M_LOG("present failed: %s", mtlb_last_error());
        break;
    }
    }
    if (op.done)
        op.done(result);
    if (op.fence)
        op.fence->Release();
    if (op.buffer) {
        op.buffer->Release();
        presents_executed_.fetch_add(1);
    }
}

void CommandQueue::present(mtlb_swapchain swapchain, Resource *buffer, UINT sync_interval)
{
    {
        // The application does not run ahead of the display: one present queued or in progress at most.
        std::unique_lock<std::mutex> lock(progress_mutex_);
        if (presents_executed_.load() != presents_posted_) {
            waiters_.fetch_add(1);
            progress_.wait(lock, [&] { return presents_executed_.load() == presents_posted_; });
            waiters_.fetch_sub(1);
        }
        ++presents_posted_;
    }
    buffer->AddRef();
    auto op = std::make_unique<Op>(Op::Kind::Present);
    op->swapchain = swapchain;
    op->buffer = buffer;
    op->texture = buffer->texture();
    op->value = sync_interval;
    dispatch(std::move(op));
}

void CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists)
{
    D3D12M_TRACE(count, lists);
    if (count && !lists)
        return;
    // The lists are validated first, so a rejected call leaves the clears pending for the next one.
    auto op = std::make_unique<Op>(Op::Kind::Submit);
    try {
        op->lists.reserve(count);
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
        op->lists.push_back(list);
    }
    // Placed render targets and depth-stencils created since the last submission are cleared first.
    op->initialized = device()->take_pending_init(op->init_stream);
    if (g_stats_enabled) {
        stat_add(Stat::Submits);
        stat_add(Stat::CommandLists, count);
        for (CommandList *list : op->lists)
            stat_add(Stat::StreamBytes, list->stream().size());
    }
    // The streams are replayed later, from the spans captured now: the lists stay alive until then, and a Reset
    // meanwhile retires the recording they point into (CommandList::reset_state).
    try {
        op->spans.reserve(op->lists.size() + 1);
    } catch (const std::bad_alloc &) {
        return;
    }
    if (!op->init_stream.empty())
        op->spans.push_back({op->init_stream.data(), op->init_stream.size()});
    for (CommandList *list : op->lists) {
        op->spans.push_back({list->stream().data(), list->stream().size()});
        list->AddRef();
        list->begin_submit();
    }
    dispatch(std::move(op));
}

HRESULT CommandQueue::signal(Fence *fence, UINT64 value, std::function<void(mtlb_result)> done)
{
    auto op = std::make_unique<Op>(Op::Kind::Signal);
    // The call must not outlive the fence's event even if another thread drops its last reference meanwhile.
    fence->AddRef();
    op->fence = fence;
    op->value = value;
    op->done = std::move(done);
    dispatch(std::move(op));
    return S_OK;
}

HRESULT CommandQueue::Signal(ID3D12Fence *fence, UINT64 value)
{
    D3D12M_TRACED_BEGIN
    auto *f = ours<Fence>(fence);
    if (!f)
        return E_INVALIDARG;
    return signal(f, value, {});
    D3D12M_TRACED_END(fence, value)
}

HRESULT CommandQueue::Wait(ID3D12Fence *fence, UINT64 value)
{
    D3D12M_TRACED_BEGIN
    auto *f = ours<Fence>(fence);
    if (!f)
        return E_INVALIDARG;
    auto op = std::make_unique<Op>(Op::Kind::Wait);
    f->AddRef();
    op->fence = f;
    op->value = value;
    dispatch(std::move(op));
    return S_OK;
    D3D12M_TRACED_END(fence, value)
}

HRESULT CommandQueue::GetTimestampFrequency(UINT64 *frequency)
{
    D3D12M_TRACED_BEGIN
    if (!frequency)
        return E_INVALIDARG;
    *frequency = mtlb_timestamp_frequency(device()->handle());
    return S_OK;
    D3D12M_TRACED_END(frequency)
}

// The GPU clock in timestamp query ticks, and the CPU clock in QueryPerformanceCounter ticks, sampled together.
HRESULT CommandQueue::GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp)
{
    D3D12M_TRACED_BEGIN
    if (!gpu_timestamp || !cpu_timestamp)
        return E_INVALIDARG;
    uint64_t gpu, cpu_mach;
    if (mtlb_gpu_clock(device()->handle(), &gpu, &cpu_mach) != MTLB_OK)
        return E_FAIL;
    *gpu_timestamp = gpu;
    *cpu_timestamp = platform_performance_counter();
    return S_OK;
    D3D12M_TRACED_END(gpu_timestamp, cpu_timestamp)
}

void CommandQueue::marker(uint32_t kind, const char *text)
{
    auto op = std::make_unique<Op>(Op::Kind::Marker);
    op->marker_kind = kind;
    if (text)
        op->text = text;
    dispatch(std::move(op));
}

// Queue markers label the work the queue has recorded so far.
void CommandQueue::SetMarker(UINT metadata, const void *data, UINT size)
{
    D3D12M_TRACE(metadata, data, size);
    marker(2, marker_text(metadata, data, size).c_str());
}

void CommandQueue::BeginEvent(UINT metadata, const void *data, UINT size)
{
    D3D12M_TRACE(metadata, data, size);
    marker(0, marker_text(metadata, data, size).c_str());
}

void CommandQueue::EndEvent()
{
    D3D12M_TRACE();
    marker(1, nullptr);
}

} // namespace d3d12m
