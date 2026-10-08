// Placement heaps: resources placed at offsets of an MTLHeap, aliasing each other where the application
// places them over the same memory.
#include "internal.h"

#include <algorithm>

namespace mtlb {

void register_buffer(Buffer *buffer)
{
    Device *device = buffer->device;
    buffer->registered = true;
    std::lock_guard<std::shared_mutex> lock(device->buffers_mutex);
    auto at = std::lower_bound(device->buffers.begin(), device->buffers.end(), buffer->gpu_address,
                               [](const std::pair<uint64_t, Buffer *> &b, uint64_t a) { return b.first < a; });
    device->buffers.insert(at, {buffer->gpu_address, buffer});
}

void unregister_buffer(Buffer *buffer)
{
    if (!buffer->registered)
        return;
    Device *device = buffer->device;
    std::lock_guard<std::shared_mutex> lock(device->buffers_mutex);
    auto &buffers = device->buffers;
    // Buffers placed over each other in a heap can share a start address: remove this very buffer.
    auto at = std::lower_bound(buffers.begin(), buffers.end(), buffer->gpu_address,
                               [](const std::pair<uint64_t, Buffer *> &b, uint64_t a) { return b.first < a; });
    for (; at != buffers.end() && at->first == buffer->gpu_address; ++at) {
        if (at->second == buffer) {
            buffers.erase(at);
            return;
        }
    }
}

MTLTextureDescriptor *make_texture_descriptor(const mtlb_texture_desc *desc, bool untracked, std::string *error)
{
    const bool depth_stencil = desc->usage & MTLB_TEXTURE_USAGE_DEPTH_STENCIL;
    MTLPixelFormat pixel_format = to_texture_pixel_format(desc->format, depth_stencil);
    if (pixel_format == MTLPixelFormatInvalid) {
        *error = "unsupported texture format " + std::to_string(desc->format);
        return nil;
    }

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = pixel_format;
    td.width = desc->width;
    td.height = desc->dimension == MTLB_TEXTURE_1D ? 1 : desc->height;
    td.mipmapLevelCount = desc->mip_levels ? desc->mip_levels : 1;
    td.sampleCount = desc->sample_count ? desc->sample_count : 1;
    td.storageMode = desc->storage == MTLB_STORAGE_SHARED ? MTLStorageModeShared : MTLStorageModePrivate;
    if (untracked)
        td.hazardTrackingMode = MTLHazardTrackingModeUntracked;

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
    return td;
}

} // namespace mtlb

using namespace mtlb;

extern "C" {

mtlb_result mtlb_buffer_size_align(mtlb_device handle, uint64_t size, mtlb_storage storage, mtlb_size_align *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || size == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    const MTLResourceOptions options = (storage == MTLB_STORAGE_SHARED ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate)
                                       | MTLResourceHazardTrackingModeUntracked;
    const MTLSizeAndAlign sa = [device->device heapBufferSizeAndAlignWithLength:size options:options];
    *out = {sa.size, sa.align};
    return MTLB_OK;
}

mtlb_result mtlb_texture_size_align(mtlb_device handle, const mtlb_texture_desc *desc, mtlb_size_align *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::string error;
    MTLTextureDescriptor *td = make_texture_descriptor(desc, true, &error);
    if (!td)
        return fail(MTLB_ERROR_UNSUPPORTED, error);
    const MTLSizeAndAlign sa = [device->device heapTextureSizeAndAlignWithDescriptor:td];
    *out = {sa.size, sa.align};
    return MTLB_OK;
}

mtlb_result mtlb_heap_create(mtlb_device handle, uint64_t size, mtlb_storage storage, mtlb_heap *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || size == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = storage == MTLB_STORAGE_SHARED ? MTLStorageModeShared : MTLStorageModePrivate;
    hd.size = size;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    id<MTLHeap> mtl_heap = [device->device newHeapWithDescriptor:hd];
    if (!mtl_heap)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newHeapWithDescriptor failed");
    device->add_resident(mtl_heap);
    auto *heap = new Heap{device, mtl_heap, storage};
    if (size <= device->device.maxBufferLength) {
        const MTLResourceOptions options = (storage == MTLB_STORAGE_SHARED ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate)
                                           | MTLResourceHazardTrackingModeUntracked;
        id<MTLBuffer> whole = [mtl_heap newBufferWithLength:size options:options offset:0];
        if (whole) {
            heap->alias = new Buffer(device, whole, whole.gpuAddress, size);
            heap->alias->placed = true;
            heap->alias->whole_heap = true;
            register_buffer(heap->alias);
        }
    }
    *out = to_handle(heap);
    return MTLB_OK;
}

void mtlb_heap_destroy(mtlb_heap handle)
{
    Heap *heap = from_handle<Heap>(handle);
    if (!heap)
        return;
    heap->device->remove_resident(heap->heap);
    if (heap->alias) {
        unregister_buffer(heap->alias);
        delete heap->alias;
    }
    delete heap;
}

mtlb_result mtlb_buffer_create_in_heap(mtlb_heap handle, uint64_t offset, uint64_t size, mtlb_buffer *out,
                                       mtlb_buffer_info *info)
{
    Heap *heap = from_handle<Heap>(handle);
    if (!heap || !out || size == 0)
        return MTLB_ERROR_INVALID_ARGUMENT;
    const MTLResourceOptions options = (heap->storage == MTLB_STORAGE_SHARED ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate)
                                       | MTLResourceHazardTrackingModeUntracked;
    id<MTLBuffer> mtl_buffer = [heap->heap newBufferWithLength:size options:options offset:offset];
    if (!mtl_buffer)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newBufferWithLength:options:offset: failed (offset misaligned or heap full)");
    auto *buffer = new Buffer(heap->device, mtl_buffer, mtl_buffer.gpuAddress, size);
    buffer->placed = true;
    // The heap's own buffer stands for it in the address table when the addresses agree.
    if (!heap->alias || mtl_buffer.gpuAddress != heap->alias->gpu_address + offset)
        register_buffer(buffer);
    if (info) {
        info->cpu_ptr = heap->storage == MTLB_STORAGE_SHARED ? mtl_buffer.contents : nullptr;
        info->gpu_address = buffer->gpu_address;
        info->size = size;
    }
    *out = to_handle(buffer);
    return MTLB_OK;
}

mtlb_result mtlb_texture_create_in_heap(mtlb_heap handle, uint64_t offset, const mtlb_texture_desc *desc,
                                        mtlb_texture *out, mtlb_texture_info *info)
{
    Heap *heap = from_handle<Heap>(handle);
    if (!heap || !desc || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::string error;
    MTLTextureDescriptor *td = make_texture_descriptor(desc, true, &error);
    if (!td)
        return fail(MTLB_ERROR_UNSUPPORTED, error);
    td.storageMode = heap->storage == MTLB_STORAGE_SHARED ? MTLStorageModeShared : MTLStorageModePrivate;
    id<MTLTexture> mtl_texture = [heap->heap newTextureWithDescriptor:td offset:offset];
    if (!mtl_texture)
        return fail(MTLB_ERROR_OUT_OF_MEMORY, "newTextureWithDescriptor:offset: failed (offset misaligned or heap full)");
    auto *texture = new Texture(heap->device, mtl_texture, static_cast<mtlb_format>(desc->format));
    texture->placed = true;
    if (info)
        info->resource_id = mtl_texture.gpuResourceID._impl;
    *out = to_handle(texture);
    return MTLB_OK;
}

} // extern "C"
