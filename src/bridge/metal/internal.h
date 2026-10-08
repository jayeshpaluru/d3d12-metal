// Backend-internal object definitions. Public handles are pointers to these
// structs cast to uint64_t.
#pragma once

#import <Metal/Metal.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "bridge/mtlb.h"

namespace mtlb {

struct Buffer;

struct Device {
    id<MTLDevice> device;
    id<MTLResidencySet> residency;
    std::atomic<bool> residency_dirty{false};

    // GPU address -> buffer, for resolving D3D12-style virtual addresses.
    std::mutex buffers_mutex;
    std::map<uint64_t, Buffer *> buffers;
};

struct Buffer {
    Device *device;
    id<MTLBuffer> buffer;
    uint64_t gpu_address;
    uint64_t size;
};

struct Texture {
    Device *device;
    id<MTLTexture> texture;
    mtlb_format format;

    // Pixel-format views, created on first use and kept for the texture's life
    // (command buffers do not retain what they reference).
    std::mutex views_mutex;
    std::map<uint32_t, id<MTLTexture>> views;
};

struct Pipeline {
    id<MTLRenderPipelineState> state;
    id<MTLDepthStencilState> depth_stencil;  // nil when depth/stencil is unused
    MTLCullMode cull_mode;
    MTLWinding winding;
    MTLTriangleFillMode fill_mode;
    MTLDepthClipMode depth_clip;
    float depth_bias;
    float slope_scaled_depth_bias;
    float depth_bias_clamp;
};

struct Queue {
    Device *device = nullptr;
    id<MTLCommandQueue> queue;

    // Guards the open command buffer: submits, signals and waits append to it
    // and it is committed lazily (see queue.mm).
    std::mutex mutex;
    id<MTLCommandBuffer> open = nil;
    uint32_t open_submits = 0;
};

struct Event {
    id<MTLSharedEvent> event;
};

template <typename T>
T *from_handle(uint64_t handle)
{
    return reinterpret_cast<T *>(static_cast<uintptr_t>(handle));
}

template <typename T>
uint64_t to_handle(T *object)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
}

// Records the failure description returned by mtlb_last_error() and returns `code`.
mtlb_result fail(mtlb_result code, const std::string &message);

// Finds the buffer containing `address` and the offset of `address` inside it.
Buffer *find_buffer(Device *device, uint64_t address, uint64_t *offset);

// Commits pending residency set changes; call before submitting work.
void commit_residency(Device *device);

// Format mapping (formats.mm). Return MTLPixelFormatInvalid / MTLVertexFormatInvalid
// for formats with no equivalent.
MTLPixelFormat to_pixel_format(uint32_t format);
MTLVertexFormat to_vertex_format(uint32_t format);

} // namespace mtlb
