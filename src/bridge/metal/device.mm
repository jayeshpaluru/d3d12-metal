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
    if (it == device->buffers.begin())
        return nullptr;
    --it;
    Buffer *buffer = it->second;
    if (address - buffer->gpu_address >= buffer->size)
        return nullptr;
    *offset = address - buffer->gpu_address;
    return buffer;
}

void commit_residency(Device *device)
{
    if (device->residency_dirty.exchange(false))
        [device->residency commit];
}

static void add_resident(Device *device, id<MTLAllocation> allocation)
{
    [device->residency addAllocation:allocation];
    device->residency_dirty = true;
}

static void remove_resident(Device *device, id<MTLAllocation> allocation)
{
    [device->residency removeAllocation:allocation];
    device->residency_dirty = true;
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
    id<MTLDevice> mtl_device = nil;
    if (registry_id == 0) {
        mtl_device = MTLCreateSystemDefaultDevice();
    } else {
        for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
            if (candidate.registryID == registry_id)
                mtl_device = candidate;
        }
    }
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
    std::memset(out, 0, sizeof(*out));
    std::strncpy(out->name, device->device.name.UTF8String, sizeof(out->name) - 1);
    out->registry_id = device->device.registryID;
    out->recommended_max_working_set_size = device->device.recommendedMaxWorkingSetSize;
    out->max_buffer_length = device->device.maxBufferLength;
    out->has_unified_memory = device->device.hasUnifiedMemory;
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

    auto *buffer = new Buffer{device, mtl_buffer, mtl_buffer.gpuAddress, size};
    {
        std::lock_guard<std::shared_mutex> lock(device->buffers_mutex);
        auto at = std::lower_bound(device->buffers.begin(), device->buffers.end(), buffer->gpu_address,
                                   [](const std::pair<uint64_t, Buffer *> &b, uint64_t a) { return b.first < a; });
        device->buffers.insert(at, {buffer->gpu_address, buffer});
    }
    add_resident(device, mtl_buffer);

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
    {
        std::lock_guard<std::shared_mutex> lock(buffer->device->buffers_mutex);
        auto &buffers = buffer->device->buffers;
        auto at = std::lower_bound(buffers.begin(), buffers.end(), buffer->gpu_address,
                                   [](const std::pair<uint64_t, Buffer *> &b, uint64_t a) { return b.first < a; });
        if (at != buffers.end() && at->first == buffer->gpu_address)
            buffers.erase(at);
    }
    remove_resident(buffer->device, buffer->buffer);
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

    MTLPixelFormat pixel_format = to_pixel_format(desc->format);
    if (pixel_format == MTLPixelFormatInvalid)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported texture format " + std::to_string(desc->format));

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = pixel_format;
    td.width = desc->width;
    td.height = desc->dimension == MTLB_TEXTURE_1D ? 1 : desc->height;
    td.mipmapLevelCount = desc->mip_levels ? desc->mip_levels : 1;
    td.sampleCount = desc->sample_count ? desc->sample_count : 1;
    td.storageMode = desc->storage == MTLB_STORAGE_SHARED ? MTLStorageModeShared : MTLStorageModePrivate;

    // Metal shader converter 3 expects 1D textures to be 2D textures.
    const bool is_3d = desc->dimension == MTLB_TEXTURE_3D;
    const bool is_array = !is_3d && desc->depth_or_array_size > 1;
    if (is_3d) {
        td.textureType = MTLTextureType3D;
        td.depth = desc->depth_or_array_size;
    } else if (td.sampleCount > 1) {
        td.textureType = is_array ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
    } else {
        td.textureType = is_array ? MTLTextureType2DArray : MTLTextureType2D;
    }
    if (is_array)
        td.arrayLength = desc->depth_or_array_size;

    MTLTextureUsage usage = MTLTextureUsagePixelFormatView;
    if (desc->usage & MTLB_TEXTURE_USAGE_SHADER_READ)
        usage |= MTLTextureUsageShaderRead;
    if (desc->usage & MTLB_TEXTURE_USAGE_SHADER_WRITE)
        usage |= MTLTextureUsageShaderWrite;
    if (desc->usage & MTLB_TEXTURE_USAGE_RENDER_TARGET)
        usage |= MTLTextureUsageRenderTarget;
    td.usage = usage;

    id<MTLTexture> mtl_texture = [device->device newTextureWithDescriptor:td];
    if (!mtl_texture)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newTextureWithDescriptor failed");
    add_resident(device, mtl_texture);

    if (info)
        info->resource_id = mtl_texture.gpuResourceID._impl;
    *out = to_handle(new Texture{device, mtl_texture, static_cast<mtlb_format>(desc->format), {}, {}});
    return MTLB_OK;
}

void mtlb_texture_destroy(mtlb_texture handle)
{
    Texture *texture = from_handle<Texture>(handle);
    if (!texture)
        return;
    remove_resident(texture->device, texture->texture);
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
