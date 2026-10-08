// Device, buffer, descriptor heap, texture and event objects.
#include "internal.h"

#include <algorithm>
#include <cstring>

#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

static_assert(sizeof(mtlb_descriptor) == sizeof(IRDescriptorTableEntry), "descriptor layout");
static_assert(offsetof(mtlb_descriptor, gpu_address) == offsetof(IRDescriptorTableEntry, gpuVA), "descriptor layout");
static_assert(offsetof(mtlb_descriptor, texture_id) == offsetof(IRDescriptorTableEntry, textureViewID), "descriptor layout");
static_assert(offsetof(mtlb_descriptor, metadata) == offsetof(IRDescriptorTableEntry, metadata), "descriptor layout");

namespace mtlb {

static thread_local std::string g_last_error;

mtlb_result fail(mtlb_result code, const std::string &message)
{
    g_last_error = message;
    return code;
}

Buffer *find_buffer(Device *device, uint64_t address, uint64_t *offset)
{
    std::shared_lock<std::shared_mutex> lock(device->buffers_mutex);
    auto it = std::upper_bound(device->buffers.begin(), device->buffers.end(), address,
                               [](uint64_t a, const std::pair<uint64_t, Buffer *> &b) { return a < b.first; });
    // Buffers placed over each other in a heap overlap: the nearest start below may be a short one, so look
    // back through the buffers that start at or before the address until one covers it.
    for (int steps = 0; it != device->buffers.begin() && steps < 64; ++steps) {
        --it;
        Buffer *buffer = it->second;
        if (address - buffer->gpu_address < buffer->size) {
            *offset = address - buffer->gpu_address;
            return buffer;
        }
    }
    return nullptr;
}

void commit_residency(Device *device)
{
    if (device->residency_dirty.exchange(false))
        [device->residency commit];
}


static id<MTLDevice> find_device(uint64_t registry_id)
{
    if (registry_id == 0)
        return MTLCreateSystemDefaultDevice();
    for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
        if (candidate.registryID == registry_id)
            return candidate;
    }
    return nil;
}

static void fill_caps(id<MTLDevice> device, mtlb_device_caps *out)
{
    std::memset(out, 0, sizeof(*out));
    std::strncpy(out->name, device.name.UTF8String, sizeof(out->name) - 1);
    out->registry_id = device.registryID;
    out->recommended_max_working_set_size = device.recommendedMaxWorkingSetSize;
    out->max_buffer_length = device.maxBufferLength;
    out->has_unified_memory = device.hasUnifiedMemory;
    for (uint32_t count : {1u, 2u, 4u, 8u, 16u}) {
        if ([device supportsTextureSampleCount:count])
            out->sample_counts |= 1u << count;
    }
    // Metal has no property for it; texture descriptor validation states 2^28 texels
    // (measured on Apple GPUs: 268435456 passes, one more asserts).
    out->max_texture_buffer_width = 1ull << 28;
}

} // namespace mtlb

using namespace mtlb;

extern "C" {

const char *mtlb_last_error(void)
{
    return g_last_error.c_str();
}

mtlb_result mtlb_device_create(uint64_t registry_id, mtlb_device *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    id<MTLDevice> mtl_device = find_device(registry_id);
    if (!mtl_device)
        return fail(MTLB_ERROR_DEVICE, "no matching Metal device available");

    MTLResidencySetDescriptor *desc = [MTLResidencySetDescriptor new];
    desc.label = @"d3d12-metal device residency";
    NSError *error = nil;
    id<MTLResidencySet> residency = [mtl_device newResidencySetWithDescriptor:desc error:&error];
    if (!residency)
        return fail(MTLB_ERROR_DEVICE, std::string("residency set: ") + error.localizedDescription.UTF8String);

    auto *device = new Device();
    device->device = mtl_device;
    device->residency = residency;
    device->listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("d3d12-metal.event-listener", DISPATCH_QUEUE_SERIAL)];
    *out = to_handle(device);
    return MTLB_OK;
}

void mtlb_device_destroy(mtlb_device handle)
{
    delete from_handle<Device>(handle);
}

mtlb_result mtlb_device_get_caps(mtlb_device handle, mtlb_device_caps *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    fill_caps(device->device, out);
    return MTLB_OK;
}

mtlb_result mtlb_query_caps(uint64_t registry_id, mtlb_device_caps *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    id<MTLDevice> device = find_device(registry_id);
    if (!device)
        return fail(MTLB_ERROR_DEVICE, "no matching Metal device available");
    fill_caps(device, out);
    return MTLB_OK;
}

mtlb_result mtlb_enum_devices(uint32_t index, mtlb_device_caps *out)
{
    if (!out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    if (index >= devices.count)
        return MTLB_ERROR_INVALID_ARGUMENT;
    fill_caps(devices[index], out);
    return MTLB_OK;
}

mtlb_result mtlb_buffer_create(mtlb_device handle, uint64_t size, mtlb_storage storage,
                               mtlb_buffer *out, mtlb_buffer_info *info)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || size == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;

    MTLResourceOptions options = storage == MTLB_STORAGE_SHARED ? MTLResourceStorageModeShared
                                                                 : MTLResourceStorageModePrivate;
    id<MTLBuffer> mtl_buffer = [device->device newBufferWithLength:size options:options];
    if (!mtl_buffer)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newBufferWithLength failed");

    auto *buffer = new Buffer(device, mtl_buffer, mtl_buffer.gpuAddress, size);
    register_buffer(buffer);
    device->add_resident(mtl_buffer);

    if (info) {
        info->cpu_ptr = storage == MTLB_STORAGE_SHARED ? mtl_buffer.contents : nullptr;
        info->gpu_address = buffer->gpu_address;
        info->size = size;
    }
    *out = to_handle(buffer);
    return MTLB_OK;
}

void mtlb_buffer_destroy(mtlb_buffer handle)
{
    Buffer *buffer = from_handle<Buffer>(handle);
    if (!buffer)
        return;
    unregister_buffer(buffer);
    if (!buffer->placed)
        buffer->device->remove_resident(buffer->buffer);
    delete buffer;
}

mtlb_result mtlb_descriptor_heap_create(mtlb_device device, uint32_t count,
                                        mtlb_buffer *out, mtlb_buffer_info *info)
{
    return mtlb_buffer_create(device, uint64_t(count) * sizeof(mtlb_descriptor), MTLB_STORAGE_SHARED, out, info);
}

mtlb_result mtlb_texture_create(mtlb_device handle, const mtlb_texture_desc *desc,
                                mtlb_texture *out, mtlb_texture_info *info)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;

    std::string error;
    MTLTextureDescriptor *td = make_texture_descriptor(desc, false, &error);
    if (!td)
        return fail(MTLB_ERROR_UNSUPPORTED, error);
    id<MTLTexture> mtl_texture = [device->device newTextureWithDescriptor:td];
    if (!mtl_texture)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newTextureWithDescriptor failed");
    device->add_resident(mtl_texture);

    if (info)
        info->resource_id = mtl_texture.gpuResourceID._impl;
    *out = to_handle(new Texture(device, mtl_texture, static_cast<mtlb_format>(desc->format)));
    return MTLB_OK;
}

void mtlb_texture_destroy(mtlb_texture handle)
{
    Texture *texture = from_handle<Texture>(handle);
    if (!texture)
        return;
    if (!texture->placed)
        texture->device->remove_resident(texture->texture);
    delete texture;
}

mtlb_result mtlb_event_create(mtlb_device handle, uint64_t initial_value, mtlb_event *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    id<MTLSharedEvent> event = [device->device newSharedEvent];
    if (!event)
        return fail(MTLB_ERROR_DEVICE, "newSharedEvent failed");
    event.signaledValue = initial_value;
    *out = to_handle(new Event{device, event});
    return MTLB_OK;
}

void mtlb_event_destroy(mtlb_event handle)
{
    delete from_handle<Event>(handle);
}

uint64_t mtlb_event_completed_value(mtlb_event handle)
{
    return from_handle<Event>(handle)->event.signaledValue;
}

void mtlb_event_signal_cpu(mtlb_event handle, uint64_t value)
{
    from_handle<Event>(handle)->event.signaledValue = value;
}

mtlb_result mtlb_event_wait_cpu(mtlb_event handle, uint64_t value, uint64_t timeout_ms)
{
    Event *event = from_handle<Event>(handle);
    if (!event)
        return MTLB_ERROR_INVALID_ARGUMENT;
    return [event->event waitUntilSignaledValue:value timeoutMS:timeout_ms] ? MTLB_OK : MTLB_ERROR_TIMEOUT;
}

} // extern "C"
