// Backend-internal object definitions. Public handles are pointers to these
// structs cast to uint64_t.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <metal_irconverter/metal_irconverter.h>

#include "bridge/mtlb.h"

namespace mtlb {

struct Buffer;

// (type, pixel format, first mip, mips, first slice, slices, swizzle) of a texture view.
using ViewKey = std::array<uint32_t, 7>;
// Sampler description fields that matter to an MTLSamplerState (the LOD bias lives in the descriptor).
using SamplerKey = std::array<uint32_t, 11>;

// A converted shader stage: its Metal function and its reflection.
struct ShaderStage {
    id<MTLFunction> function = nil;
    std::shared_ptr<IRShaderReflection> reflection;  // kept for stage-in synthesis
    uint32_t num_vertex_inputs = 0;
    uint32_t threadgroup_size[3] = {1, 1, 1};  // compute stages

    // Vertex stage-in functions synthesized for this shader, by serialized input layout.
    std::mutex stage_in_mutex;
    std::map<std::string, id<MTLFunction>> stage_ins;
};

// (hash of the DXIL, DXIL size, root signature id, IRShaderStage, entry point)
using ShaderKey = std::tuple<uint64_t, uint64_t, uint64_t, uint32_t, std::string>;

// Depth/stencil description fields that matter to an MTLDepthStencilState.
using DepthStencilKey = std::array<uint32_t, 13>;

struct Device {
    id<MTLDevice> device;
    id<MTLResidencySet> residency;
    std::atomic<bool> residency_dirty{false};

    // Listener shared by all events' notifications (see notify.mm).
    MTLSharedEventListener *listener;

    // Converted shaders, shared by every pipeline using the same DXIL and root
    // signature, and depth-stencil states shared by equal descriptions.
    std::mutex shaders_mutex;
    std::map<ShaderKey, std::shared_ptr<const ShaderStage>> shaders;
    std::mutex depth_stencil_mutex;
    std::map<DepthStencilKey, id<MTLDepthStencilState>> depth_stencil_states;

    // The backend's own compute kernels (kernels.mm).
    std::mutex kernels_mutex;
    id<MTLLibrary> kernel_library = nil;
    std::map<std::string, id<MTLComputePipelineState>> kernels;

    // Samplers shared by equal descriptions.
    std::mutex sampler_mutex;
    std::map<SamplerKey, id<MTLSamplerState>> samplers;

    // Resources behind null descriptors, created on first use, by mtlb_null_kind.
    static constexpr uint32_t kNullKinds = 16;
    std::mutex null_mutex;
    id<MTLBuffer> null_buffer = nil;
    id<MTLTexture> null_textures[kNullKinds] = {};

    void add_resident(id<MTLAllocation> allocation)
    {
        [residency addAllocation:allocation];
        residency_dirty = true;
    }
    void remove_resident(id<MTLAllocation> allocation)
    {
        [residency removeAllocation:allocation];
        residency_dirty = true;
    }

    // Buffers sorted by GPU address, for resolving D3D12-style virtual
    // addresses. Lookups far outnumber creations.
    std::shared_mutex buffers_mutex;
    std::vector<std::pair<uint64_t, Buffer *>> buffers;
};

struct QueryHeap {
    Device *device;
    uint32_t kind;
    uint32_t count;
    id<MTLBuffer> results;                    // occlusion counts, 8 bytes per query
    id<MTLCounterSampleBuffer> samples;       // timestamps (nil where the GPU cannot sample them)
};

struct Heap {
    Device *device;
    id<MTLHeap> heap;
    mtlb_storage storage;
};

struct Buffer {
    Buffer(Device *d, id<MTLBuffer> b, uint64_t address, uint64_t length)
        : device(d), buffer(b), gpu_address(address), size(length) {}

    Device *device;
    id<MTLBuffer> buffer;
    uint64_t gpu_address;
    uint64_t size;
    bool placed = false;  // lives in a heap, which owns its residency

    // Texture buffer views (typed views and UAV counters), created on first use and kept for the
    // buffer's life: (byte offset, pixel format, texel count, writable).
    std::mutex views_mutex;
    std::map<std::tuple<uint64_t, uint32_t, uint64_t, bool>, id<MTLTexture>> texture_views;
};

struct Texture {
    Texture(Device *d, id<MTLTexture> t, mtlb_format f) : device(d), texture(t), format(f) {}

    Device *device;
    id<MTLTexture> texture;
    mtlb_format format;
    bool placed = false;

    // Pixel-format views, created on first use and kept for the texture's life
    // (command buffers do not retain what they reference).
    std::mutex views_mutex;
    std::map<uint32_t, id<MTLTexture>> views;

    // Shader-visible views (type, range, format, swizzle), kept for the texture's life.
    std::map<ViewKey, id<MTLTexture>> sampled_views;
};

struct RootSignature {
    Device *device;
    uint64_t id;  // unique for the process lifetime, so cache keys never see a reused address
    IRRootSignature *ir;
};

struct Pipeline {
    // A compute pipeline has `compute` set and none of the render state.
    id<MTLComputePipelineState> compute = nil;
    MTLSize threadgroup_size = {1, 1, 1};

    id<MTLRenderPipelineState> state;
    id<MTLDepthStencilState> depth_stencil;      // the description's depth and stencil state
    id<MTLDepthStencilState> depth_stencil_off;  // for passes without depth-stencil attachment
    MTLCullMode cull_mode;
    MTLWinding winding;
    MTLTriangleFillMode fill_mode;
    MTLDepthClipMode depth_clip;
    float depth_bias;
    float slope_scaled_depth_bias;
    float depth_bias_clamp;

    // The pipeline is built for the depth-stencil format of its description. A pass with another (or
    // no) depth-stencil attachment gets a variant, built when first needed.
    MTLRenderPipelineDescriptor *descriptor = nil;
    MTLPixelFormat depth_format = MTLPixelFormatInvalid;
    MTLPixelFormat stencil_format = MTLPixelFormatInvalid;
    std::mutex variants_mutex;
    std::map<uint64_t, id<MTLRenderPipelineState>> variants;
    Device *device = nullptr;

    // The render pipeline state for a pass whose attachments have these pixel formats; nil with
    // fail() set when the variant cannot be built.
    id<MTLRenderPipelineState> state_for(MTLPixelFormat depth, MTLPixelFormat stencil);
};

struct Queue {
    Device *device = nullptr;
    id<MTLCommandQueue> queue;

    // Guards the open command buffer: a wait is encoded into it and submits and
    // signals append to it and commit it (see queue.mm).
    std::mutex mutex;
    id<MTLCommandBuffer> open = nil;
    std::atomic<uint64_t> render_passes{0};

    // Orders encoders on this queue: resources are reached through GPU addresses and descriptor
    // tables, which Metal's automatic hazard tracking does not see, so every encoder updates
    // the fence when it ends and the next one waits for it (queue.mm).
    int debug_depth = 0;  // debug groups opened on the open command buffer by mtlb_queue_marker

    id<MTLFence> fence = nil;
    bool fence_pending = false;  // an encoder updated the fence and no later encoder has waited yet
};

// A CAMetalLayer attached to an application window, and what it takes to put a
// back buffer on it (swapchain.mm).
struct Swapchain {
    Device *device;
    CAMetalLayer *layer;
    id<MTLLibrary> library;  // present_vs / present_fs

    std::mutex mutex;        // guards pipeline and pixel_format against a resize
    id<MTLRenderPipelineState> pipeline;  // fullscreen triangle sampling the back buffer
    MTLPixelFormat pixel_format;          // of the layer's drawables

    ~Swapchain();
    bool layer_owned = false;  // the layer came from the provider and goes back to its releaser

    std::atomic<bool> display_sync{true};

    // Debug aid: D3D12METAL_DUMP_PRESENT=<file.png> writes what the Nth present
    // (D3D12METAL_DUMP_PRESENT_FRAME, default 30) drew onto the drawable.
    std::string dump_path;
    uint64_t dump_frame = 0;
    std::atomic<uint64_t> presents{0};
};

struct Event {
    Device *device;
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

// Adds `buffer` to / removes it from the table that resolves GPU addresses.
void register_buffer(Buffer *buffer);
void unregister_buffer(Buffer *buffer);

// The Metal descriptor for a texture of `desc`; nil (with `error` set) for formats Metal cannot do.
MTLTextureDescriptor *make_texture_descriptor(const mtlb_texture_desc *desc, bool untracked, std::string *error);

// Finds the buffer containing `address` and the offset of `address` inside it.
Buffer *find_buffer(Device *device, uint64_t address, uint64_t *offset);

// Commits pending residency set changes; call before submitting work.
void commit_residency(Device *device);

// A drawable of a swap chain with the pipeline that draws onto it.
struct Drawable {
    id<CAMetalDrawable> drawable = nil;  // nil when none was available (a hidden window)
    id<MTLRenderPipelineState> pipeline = nil;
};

// Takes the swap chain's next drawable and applies the sync interval. Blocks while
// every drawable is in flight, so call it without holding locks.
Drawable acquire_drawable(Swapchain *swapchain, uint32_t sync_interval);

// Encodes drawing `texture` onto the drawable and presenting it into `command_buffer`.
void encode_present(Queue *queue, id<MTLCommandBuffer> command_buffer, Swapchain *swapchain, Texture *texture,
                    const Drawable &drawable);

// The backend's own compute kernel `name` (kernels.mm); nil with fail() set when it cannot be built.
id<MTLComputePipelineState> internal_kernel(Device *device, NSString *name);

// The Metal texture a shader-visible view of `texture` described by `desc` refers to (the texture itself when
// the view changes nothing); nil with fail() set on error. Cached on the texture.
id<MTLTexture> texture_view_object(Texture *texture, const mtlb_texture_view_desc *desc);

// Format mapping (formats.mm). Return MTLPixelFormatInvalid / MTLVertexFormatInvalid
// for formats with no equivalent.
MTLPixelFormat to_pixel_format(uint32_t format);
MTLVertexFormat to_vertex_format(uint32_t format);
// The pixel format a texture of `format` is created with; `depth_stencil_usage` picks the depth variant
// of R32_TYPELESS and R16_TYPELESS.
MTLPixelFormat to_texture_pixel_format(uint32_t format, bool depth_stencil_usage);
// The pixel format of a view of a texture created as `base`, viewed as `view_format`.
MTLPixelFormat to_view_pixel_format(MTLPixelFormat base, uint32_t view_format);

} // namespace mtlb
