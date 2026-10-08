// Query heaps and GPU clocks.
#include "bridge/metal/log.h"
#include "internal.h"

#include <mach/mach_time.h>

#include <algorithm>

using namespace mtlb;

namespace mtlb {

id<MTLCounterSampleBuffer> sample_buffer(QueryHeap *heap, uint32_t index)
{
    const uint32_t chunk = index / kSamplesPerBuffer;
    if (chunk >= heap->samples.size())
        return nil;
    std::lock_guard<std::mutex> lock(heap->samples_mutex);
    if (!heap->samples[chunk] && !heap->samples_failed[chunk]) {
        ++heap->sample_attempts;
        id<MTLCounterSet> timestamp_set = nil;
        for (id<MTLCounterSet> set in heap->device->device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                timestamp_set = set;
        }
        MTLCounterSampleBufferDescriptor *descriptor = [MTLCounterSampleBufferDescriptor new];
        descriptor.counterSet = timestamp_set;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.sampleCount = std::min(heap->count - chunk * kSamplesPerBuffer, kSamplesPerBuffer);
        NSError *error = nil;
        if (!heap->test_fail_samples)
            heap->samples[chunk] = [heap->device->device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        if (!heap->samples[chunk]) {
            // Tried once per buffer: the failure is remembered, and logged once.
            heap->samples_failed[chunk] = 1;
            backend_log("timestamp queries unavailable: %s", error ? error.localizedDescription.UTF8String : "injected failure");
        }
    }
    return heap->samples[chunk];
}

} // namespace mtlb

extern "C" {

mtlb_result mtlb_query_heap_create(mtlb_device handle, uint32_t kind, uint32_t count, mtlb_query_heap *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || count == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    auto *heap = new QueryHeap{device, kind, count, nil, {}, {}, {}};
    if (kind == MTLB_QUERY_OCCLUSION || kind == MTLB_QUERY_BINARY_OCCLUSION) {
        // The visibility result buffer of render passes: result slots of 8 bytes per query.
        heap->slots_used.assign(count, 0);
        heap->results = [device->device newBufferWithLength:uint64_t(count) * kQuerySlots * 8 options:MTLResourceStorageModeShared];
        if (!heap->results) {
            delete heap;
            return fail(MTLB_ERROR_OUT_OF_MEMORY, "visibility result buffer");
        }
        device->add_resident(heap->results);
    } else if (kind == MTLB_QUERY_TIMESTAMP) {
        // Timestamps are sampled at the boundaries of encoders; GPUs that cannot do that report zeros.
        id<MTLCounterSet> timestamp_set = nil;
        for (id<MTLCounterSet> set in device->device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
                timestamp_set = set;
        }
        if (timestamp_set && [device->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            heap->samples.resize((count + kSamplesPerBuffer - 1) / kSamplesPerBuffer);  // made by sample_buffer()
            heap->samples_failed.assign(heap->samples.size(), 0);
        } else {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                backend_log("this GPU cannot sample timestamps at encoder boundaries; timestamp queries read zero");
        }
    }
    *out = to_handle(heap);
    return MTLB_OK;
}

uint64_t mtlb_query_heap_test_sample_attempts(mtlb_query_heap handle, int fail_creation)
{
    auto *heap = from_handle<QueryHeap>(handle);
    if (!heap)
        return 0;
    std::lock_guard<std::mutex> lock(heap->samples_mutex);
    if (fail_creation >= 0)
        heap->test_fail_samples = fail_creation != 0;
    return heap->sample_attempts;
}

void mtlb_query_heap_destroy(mtlb_query_heap handle)
{
    auto *heap = from_handle<QueryHeap>(handle);
    if (!heap)
        return;
    if (heap->results)
        heap->device->remove_resident(heap->results);
    delete heap;
}

uint64_t mtlb_timestamp_frequency(mtlb_device)
{
    // GPU timestamps share the scale of mach_absolute_time.
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return uint64_t(1000000000ull) * timebase.denom / timebase.numer;
}

mtlb_result mtlb_gpu_clock(mtlb_device handle, uint64_t *gpu_ticks, uint64_t *cpu_ticks)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !gpu_ticks || !cpu_ticks)
        return MTLB_ERROR_INVALID_ARGUMENT;
    MTLTimestamp cpu = 0, gpu = 0;
    [device->device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    *gpu_ticks = gpu;
    *cpu_ticks = cpu;
    return MTLB_OK;
}

} // extern "C"
