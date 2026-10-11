// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12CommandQueue backed by an mtlb queue.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge/mtlb.h"
#include "d3d12/object.h"

namespace d3d12m {

class CommandList;
class Fence;
class Resource;

// The queue's work reaches the backend in API order from a worker thread of its own (created on first use), so the
// application's thread does not spend its time replaying command streams: an operation is a heap record whose
// address is posted to the worker's notification queue (mtlb_notify_post; the queue's FIFO order is the submission
// order, and a wake-up through the bridge is cheaper under Wine than a Windows event). With async_submit=0 the
// operations run on the calling thread instead.

class CommandQueue final : public ChildImpl<ID3D12CommandQueue> {
public:
    static HRESULT create(Device *device, const D3D12_COMMAND_QUEUE_DESC &desc, REFIID riid, void **out);

    // The backend queue, once every operation enqueued so far has reached it (and been committed).
    mtlb_queue handle()
    {
        flush();
        return queue_;
    }
    // Blocks until every operation enqueued so far has been executed and committed to the GPU (exported: handle() is
    // used by tests).
    __attribute__((visibility("default"))) void flush();
    // Enqueues the presentation of `buffer` (kept alive until then). Blocks while an earlier present of this queue
    // has not been executed yet, so the application cannot run ahead of the display. Failures are only logged.
    void present(mtlb_swapchain swapchain, Resource *buffer, UINT sync_interval);
    // As Signal, and `done` runs after the bridge call with its result.
    HRESULT signal(Fence *fence, UINT64 value, std::function<void(mtlb_result)> done);
    Device *device() const { return ChildImpl::device(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(this, riid, out);
    }

    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *, UINT, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, ID3D12Heap *, UINT, const D3D12_TILE_RANGE_FLAGS *, const UINT *, const UINT *, D3D12_TILE_MAPPING_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *, D3D12_TILE_MAPPING_FLAGS) override { D3D12M_STUB_LOG(); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) override;
    D3D12M_AGGREGATE_RETURN(D3D12_COMMAND_QUEUE_DESC, GetDesc, desc_)

private:
    struct Op;
    explicit CommandQueue(Device *device) : ChildImpl(device) {}
    ~CommandQueue() override;

    // Runs the operation, now or on the worker; takes ownership.
    void dispatch(std::unique_ptr<Op> op);
    void execute(Op &op);
    void run();
    void marker(uint32_t kind, const char *text);
    D3D12_COMMAND_QUEUE_DESC desc_{};
    mtlb_queue queue_ = 0;

    bool async_ = false;                  // decided once, with the worker
    std::once_flag worker_started_;
    mtlb_notify notify_ = 0;
    std::thread worker_;
    std::atomic<uint64_t> posted_{0};     // operations enqueued
    std::atomic<uint64_t> executed_{0};   // operations executed
    uint64_t presents_posted_ = 0;        // guarded by progress_mutex_
    std::atomic<uint64_t> presents_executed_{0};
    std::atomic<uint32_t> waiters_{0};    // threads blocked on progress_ (flush, present): the worker wakes them
    std::mutex progress_mutex_;
    std::condition_variable progress_;
};

} // namespace d3d12m
