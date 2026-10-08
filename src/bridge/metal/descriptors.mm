// What goes into descriptors: texture views, typed buffer views (texture buffers),
// null descriptors and samplers, in the layout the Metal shader converter reads
// (IRDescriptorTableEntry; see docs/ARCHITECTURE.md).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

namespace {

using namespace mtlb;

// The metadata word of a buffer descriptor (IRDescriptorTableGetBufferMetadata):
// byte size in the low 32 bits, texture buffer view padding (elements) in bits 32..39,
// "typed buffer" in bit 63.
uint64_t buffer_metadata(uint64_t size_bytes, uint32_t padding_elements, bool typed)
{
    IRBufferView view = {};
    view.bufferSize = std::min<uint64_t>(size_bytes, 0xffffffffull);
    view.textureViewOffsetInElements = padding_elements;
    view.typedBuffer = typed;
    return IRDescriptorTableGetBufferMetadata(&view);
}

void log_once(std::atomic<bool> &flag, const char *message)
{
    if (!flag.exchange(true))
        std::fprintf(stderr, "d3d12-metal: %s\n", message);
}

MTLTextureType to_texture_type(uint32_t type)
{
    switch (type) {
    case MTLB_VIEW_2D_ARRAY: return MTLTextureType2DArray;
    case MTLB_VIEW_2D_MS: return MTLTextureType2DMultisample;
    case MTLB_VIEW_2D_MS_ARRAY: return MTLTextureType2DMultisampleArray;
    case MTLB_VIEW_3D: return MTLTextureType3D;
    case MTLB_VIEW_CUBE: return MTLTextureTypeCube;
    case MTLB_VIEW_CUBE_ARRAY: return MTLTextureTypeCubeArray;
    default: return MTLTextureType2D;
    }
}

// D3D12 Shader4ComponentMapping to a Metal swizzle: 0-3 pick a source component, 4 forces
// zero and 5 forces one.
MTLTextureSwizzle to_swizzle(uint32_t mapping, int component)
{
    switch ((mapping >> (3 * component)) & 7) {
    case 0: return MTLTextureSwizzleRed;
    case 1: return MTLTextureSwizzleGreen;
    case 2: return MTLTextureSwizzleBlue;
    case 3: return MTLTextureSwizzleAlpha;
    case 4: return MTLTextureSwizzleZero;
    default: return MTLTextureSwizzleOne;
    }
}

constexpr uint32_t kIdentityMapping = 0x688;  // D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING without the marker bit

MTLSamplerAddressMode to_address_mode(uint32_t mode)
{
    switch (mode) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

MTLSamplerMinMagFilter to_filter(uint32_t filter)
{
    return filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

MTLCompareFunction to_compare_function(uint32_t func)
{
    switch (func) {
    case MTLB_COMPARE_NEVER: return MTLCompareFunctionNever;
    case MTLB_COMPARE_LESS: return MTLCompareFunctionLess;
    case MTLB_COMPARE_EQUAL: return MTLCompareFunctionEqual;
    case MTLB_COMPARE_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case MTLB_COMPARE_GREATER: return MTLCompareFunctionGreater;
    case MTLB_COMPARE_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case MTLB_COMPARE_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

uint32_t float_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A 1x1 (or 1x1x1) texture of the shape `kind` asks for, zero-filled.
id<MTLTexture> make_null_texture(Device *device, uint32_t kind)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.width = 1;
    td.height = 1;
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    switch (kind) {
    case MTLB_NULL_TEXTURE_2D: td.textureType = MTLTextureType2D; break;
    case MTLB_NULL_TEXTURE_2D_ARRAY: td.textureType = MTLTextureType2DArray; td.arrayLength = 1; break;
    case MTLB_NULL_TEXTURE_2D_MS: td.textureType = MTLTextureType2DMultisample; td.sampleCount = 4; break;
    case MTLB_NULL_TEXTURE_2D_MS_ARRAY: td.textureType = MTLTextureType2DMultisampleArray; td.sampleCount = 4; td.arrayLength = 1; break;
    case MTLB_NULL_TEXTURE_3D: td.textureType = MTLTextureType3D; td.depth = 1; break;
    case MTLB_NULL_TEXTURE_CUBE: td.textureType = MTLTextureTypeCube; break;
    case MTLB_NULL_TEXTURE_CUBE_ARRAY: td.textureType = MTLTextureTypeCubeArray; td.arrayLength = 1; break;
    case MTLB_NULL_UAV_TEXTURE_2D: td.textureType = MTLTextureType2D; break;
    case MTLB_NULL_UAV_TEXTURE_2D_ARRAY: td.textureType = MTLTextureType2DArray; td.arrayLength = 1; break;
    case MTLB_NULL_UAV_TEXTURE_3D: td.textureType = MTLTextureType3D; td.depth = 1; break;
    default: return nil;
    }
    if (kind >= MTLB_NULL_UAV_TEXTURE_2D) {
        td.pixelFormat = MTLPixelFormatRGBA8Unorm;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    }
    id<MTLTexture> texture = [device->device newTextureWithDescriptor:td];
    if (texture)
        device->add_resident(texture);
    return texture;
}

} // namespace

namespace mtlb {

// A texture buffer view of `count` elements of `pixel_format` starting `offset` bytes into `buffer`
// (already aligned to the format's linear texture alignment), cached on the buffer.
static id<MTLTexture> texture_buffer_view(Buffer *buffer, uint64_t offset, MTLPixelFormat pixel_format,
                                          uint64_t count, uint32_t bytes_per_element, bool writable)
{
    const auto key = std::make_tuple(offset, static_cast<uint32_t>(pixel_format), count, writable);
    std::lock_guard<std::mutex> lock(buffer->views_mutex);
    auto it = buffer->texture_views.find(key);
    if (it != buffer->texture_views.end())
        return it->second;

    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureTypeTextureBuffer;
    td.pixelFormat = pixel_format;
    td.width = count;
    td.height = 1;
    td.storageMode = buffer->buffer.storageMode;
    td.cpuCacheMode = buffer->buffer.cpuCacheMode;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (writable)
        td.usage |= MTLTextureUsageShaderWrite;
    const NSUInteger alignment = [buffer->device->device minimumLinearTextureAlignmentForPixelFormat:pixel_format];
    uint64_t bytes_per_row = (count * bytes_per_element + alignment - 1) / alignment * alignment;
    if (offset + bytes_per_row > buffer->size)
        bytes_per_row = count * bytes_per_element;  // the rounded row would run past the buffer
    id<MTLTexture> texture = [buffer->buffer newTextureWithDescriptor:td offset:offset bytesPerRow:bytes_per_row];
    if (texture)
        buffer->texture_views.emplace(key, texture);
    return texture;
}

} // namespace mtlb

extern "C" {

mtlb_result mtlb_texture_view(mtlb_texture handle, const mtlb_texture_view_desc *desc, uint64_t *out)
{
    Texture *texture = from_handle<Texture>(handle);
    if (!texture || !desc || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    id<MTLTexture> base = texture->texture;

    const MTLTextureType type = to_texture_type(desc->type);
    const MTLPixelFormat pixel_format =
        desc->format ? to_view_pixel_format(base.pixelFormat, desc->format) : base.pixelFormat;
    if (pixel_format == MTLPixelFormatInvalid)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported view format " + std::to_string(desc->format));

    const uint32_t levels = static_cast<uint32_t>(base.mipmapLevelCount);
    const uint32_t layers = static_cast<uint32_t>(std::max<NSUInteger>(base.arrayLength, 1));
    if (desc->first_mip >= levels || desc->first_slice >= layers)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "view range outside the texture");
    const uint32_t mip_count = std::min(desc->mip_count ? desc->mip_count : levels, levels - desc->first_mip);
    const bool is_3d = type == MTLTextureType3D;
    const uint32_t slice_count = is_3d ? 1 : std::min(desc->slice_count ? desc->slice_count : layers, layers - desc->first_slice);
    const uint32_t mapping = desc->component_mapping & 0xfff;
    const bool swizzled = desc->component_mapping != 0 && mapping != kIdentityMapping;

    if (type == base.textureType && pixel_format == base.pixelFormat && desc->first_mip == 0 && mip_count == levels
        && desc->first_slice == 0 && (is_3d || slice_count == layers) && !swizzled) {
        *out = base.gpuResourceID._impl;
        return MTLB_OK;
    }

    const ViewKey key{static_cast<uint32_t>(type), static_cast<uint32_t>(pixel_format), desc->first_mip, mip_count,
                      desc->first_slice, slice_count, swizzled ? mapping : 0u};
    std::lock_guard<std::mutex> lock(texture->views_mutex);
    auto &view = texture->sampled_views[key];
    if (!view) {
        MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsDefault;
        if (swizzled)
            channels = MTLTextureSwizzleChannelsMake(to_swizzle(mapping, 0), to_swizzle(mapping, 1), to_swizzle(mapping, 2),
                                                     to_swizzle(mapping, 3));
        view = [base newTextureViewWithPixelFormat:pixel_format textureType:type
                                            levels:NSMakeRange(desc->first_mip, mip_count)
                                            slices:NSMakeRange(desc->first_slice, slice_count)
                                           swizzle:channels];
        if (!view) {
            texture->sampled_views.erase(key);
            return fail(MTLB_ERROR_UNSUPPORTED, "texture view creation failed");
        }
    }
    *out = view.gpuResourceID._impl;
    return MTLB_OK;
}

mtlb_result mtlb_buffer_view(const mtlb_buffer_view_desc *desc, mtlb_descriptor *out)
{
    Buffer *buffer = desc ? from_handle<Buffer>(desc->buffer) : nullptr;
    if (!buffer || !out || desc->offset > buffer->size)
        return MTLB_ERROR_INVALID_ARGUMENT;
    Device *device = buffer->device;
    const uint64_t available = buffer->size - desc->offset;
    uint64_t size_bytes = std::min(desc->size, available);
    *out = {buffer->gpu_address + desc->offset, 0, 0};
    uint32_t padding = 0;

    if (desc->format == 0) {
        // Raw or structured: shaders address the memory directly.
        if (desc->counter_buffer) {
            Buffer *counter = from_handle<Buffer>(desc->counter_buffer);
            if (!counter || desc->counter_offset + 4 > counter->size)
                return fail(MTLB_ERROR_INVALID_ARGUMENT, "UAV counter outside its buffer");
            const NSUInteger alignment = [device->device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Uint];
            const uint64_t aligned = desc->counter_offset / alignment * alignment;
            padding = static_cast<uint32_t>((desc->counter_offset - aligned) / 4);
            id<MTLTexture> view = texture_buffer_view(counter, aligned, MTLPixelFormatR32Uint, 1 + padding, 4, true);
            if (!view)
                return fail(MTLB_ERROR_UNSUPPORTED, "UAV counter view creation failed");
            out->texture_id = view.gpuResourceID._impl;
        }
        out->metadata = buffer_metadata(size_bytes, padding, false);
        return MTLB_OK;
    }

    mtlb_format_info info;
    const MTLPixelFormat pixel_format = to_pixel_format(desc->format);
    if (mtlb_format_get_info(static_cast<mtlb_format>(desc->format), &info) != MTLB_OK || pixel_format == MTLPixelFormatInvalid
        || !(info.flags & MTLB_FORMAT_FLAG_BUFFER))
        return fail(MTLB_ERROR_UNSUPPORTED, "format " + std::to_string(desc->format) + " cannot be a typed buffer view");
    const uint32_t bytes = info.bytes_per_block;
    const NSUInteger alignment = [device->device minimumLinearTextureAlignmentForPixelFormat:pixel_format];
    const uint64_t aligned = desc->offset / alignment * alignment;
    const uint64_t pad_bytes = desc->offset - aligned;
    if (pad_bytes % bytes != 0 || pad_bytes / bytes > 0xff)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "typed buffer view offset is not element aligned");
    padding = static_cast<uint32_t>(pad_bytes / bytes);
    if (padding) {
        // The shader converter reads a typed buffer from the start of the texture buffer and ignores the
        // padding field, so the view starts at the aligned offset below the requested one.
        static std::atomic<bool> logged{false};
        log_once(logged, "a typed buffer view starts at an offset a texture buffer cannot start at; it starts earlier");
    }

    // A texture buffer holds at most max_texture_buffer_width texels. Views above it (the game this
    // layer targets creates 419,430,400-element views) are clamped: elements past the limit read as
    // zero instead of their contents.
    const uint64_t limit = 1ull << 28;
    uint64_t elements = std::min<uint64_t>(desc->num_elements, available / bytes);
    if (elements + padding > limit) {
        static std::atomic<bool> logged{false};
        log_once(logged, "a typed buffer view exceeds the texture buffer limit and is clamped to it");
        elements = limit - padding;
    }
    size_bytes = elements * bytes;
    id<MTLTexture> view = texture_buffer_view(buffer, aligned, pixel_format, elements + padding, bytes, true);
    if (!view)
        return fail(MTLB_ERROR_UNSUPPORTED, "typed buffer view creation failed");
    out->texture_id = view.gpuResourceID._impl;
    out->metadata = buffer_metadata(size_bytes, padding, true);
    return MTLB_OK;
}

mtlb_result mtlb_null_descriptor(mtlb_device handle, uint32_t kind, mtlb_descriptor *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out || kind == 0 || kind >= Device::kNullKinds)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(device->null_mutex);
    if (!device->null_buffer) {
        device->null_buffer = [device->device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!device->null_buffer)
            return fail(MTLB_ERROR_OUT_OF_MEMORY, "null buffer");
        device->add_resident(device->null_buffer);
    }
    if (kind == MTLB_NULL_BUFFER) {
        // Size zero: bounds-checked shader accesses read zero and write nothing.
        *out = {device->null_buffer.gpuAddress, 0, buffer_metadata(0, 0, false)};
        return MTLB_OK;
    }
    if (kind == MTLB_NULL_TYPED_BUFFER || kind == MTLB_NULL_UAV_TYPED_BUFFER) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureTypeTextureBuffer;
        td.pixelFormat = MTLPixelFormatR32Uint;
        td.width = 1;
        td.height = 1;
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
        if (!device->null_textures[kind]) {
            device->null_textures[kind] = [device->null_buffer newTextureWithDescriptor:td offset:0 bytesPerRow:256];
            if (!device->null_textures[kind])
                return fail(MTLB_ERROR_UNSUPPORTED, "null typed buffer");
        }
        *out = {device->null_buffer.gpuAddress, device->null_textures[kind].gpuResourceID._impl, buffer_metadata(0, 0, true)};
        return MTLB_OK;
    }
    if (!device->null_textures[kind]) {
        device->null_textures[kind] = make_null_texture(device, kind);
        if (!device->null_textures[kind])
            return fail(MTLB_ERROR_UNSUPPORTED, "null texture " + std::to_string(kind));
    }
    *out = {0, device->null_textures[kind].gpuResourceID._impl, 0};
    return MTLB_OK;
}

mtlb_result mtlb_sampler_create(mtlb_device handle, const mtlb_sampler_desc *desc, mtlb_descriptor *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !desc || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    if (desc->reduction) {
        static std::atomic<bool> logged{false};
        log_once(logged, "min/max sampler reduction is not supported by Metal samplers, filtering is averaged");
    }

    // Metal has three border colours: pick the nearest.
    const float *c = desc->border_color;
    MTLSamplerBorderColor border = MTLSamplerBorderColorTransparentBlack;
    if (c[0] > 0.5f || c[1] > 0.5f || c[2] > 0.5f)
        border = MTLSamplerBorderColorOpaqueWhite;
    else if (c[3] > 0.5f)
        border = MTLSamplerBorderColorOpaqueBlack;
    if (!(c[0] == 0 && c[1] == 0 && c[2] == 0 && (c[3] == 0 || c[3] == 1)) && !(c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1)) {
        static std::atomic<bool> logged{false};
        log_once(logged, "a sampler border colour other than transparent black, opaque black or opaque white is approximated");
    }

    const uint32_t anisotropy = std::clamp<uint32_t>(desc->max_anisotropy, 1, 16);
    const SamplerKey key{desc->min_filter, desc->mag_filter, desc->mip_filter, desc->address_u, desc->address_v,
                         desc->address_w, anisotropy, desc->compare_func, static_cast<uint32_t>(border),
                         float_bits(desc->min_lod), float_bits(desc->max_lod)};
    id<MTLSamplerState> sampler;
    {
        std::lock_guard<std::mutex> lock(device->sampler_mutex);
        auto it = device->samplers.find(key);
        if (it != device->samplers.end()) {
            sampler = it->second;
        } else {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = to_filter(desc->min_filter);
            sd.magFilter = to_filter(desc->mag_filter);
            sd.mipFilter = desc->mip_filter ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
            sd.sAddressMode = to_address_mode(desc->address_u);
            sd.tAddressMode = to_address_mode(desc->address_v);
            sd.rAddressMode = to_address_mode(desc->address_w);
            sd.borderColor = border;
            sd.maxAnisotropy = anisotropy;
            sd.lodMinClamp = desc->min_lod;
            sd.lodMaxClamp = std::min(desc->max_lod, 1000.0f);
            sd.compareFunction = desc->compare_func ? to_compare_function(desc->compare_func) : MTLCompareFunctionNever;
            sd.supportArgumentBuffers = YES;
            sampler = [device->device newSamplerStateWithDescriptor:sd];
            if (!sampler)
                return fail(MTLB_ERROR_UNSUPPORTED, "newSamplerStateWithDescriptor failed");
            device->samplers.emplace(key, sampler);
        }
    }
    IRDescriptorTableEntry entry;
    IRDescriptorTableSetSampler(&entry, sampler, desc->mip_lod_bias);
    *out = {entry.gpuVA, entry.textureViewID, entry.metadata};
    return MTLB_OK;
}

} // extern "C"
