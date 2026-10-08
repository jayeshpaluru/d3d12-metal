// Query heaps and GPU clocks.
#include "internal.h"

#include <mach/mach_time.h>

using namespace mtlb;

extern "C" {

mtlb_result mtlb_query_heap_create(mtlb_device handle, uint32_t kind, uint32_t count, mtlb_query_heap *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || count == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    auto *heap = new QueryHeap{device, kind, count, nil, nil, {}};
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
            MTLCounterSampleBufferDescriptor *descriptor = [MTLCounterSampleBufferDescriptor new];
            descriptor.counterSet = timestamp_set;
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.sampleCount = count;
            NSError *error = nil;
            heap->samples = [device->device newCounterSampleBufferWithDescriptor:descriptor error:&error];
            if (!heap->samples)
                fprintf(stderr, "d3d12-metal: timestamp queries unavailable: %s\n", error.localizedDescription.UTF8String);
        } else {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                fprintf(stderr, "d3d12-metal: this GPU cannot sample timestamps at encoder boundaries; timestamp queries read zero\n");
        }
    }
    *out = to_handle(heap);
    return MTLB_OK;
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
