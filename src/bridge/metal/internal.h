// SPDX-License-Identifier: LGPL-2.1-or-later
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
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include "bridge/mtlb.h"
#include "disk_cache.h"
#include "hud.h"

namespace mtlb {

// Backend counters (mtlb_stats): relaxed atomic increments, a few nanoseconds each.
enum Stat : unsigned { kStatSubmits, kStatCommandBuffers, kStatRenderEncoders, kStatComputeEncoders, kStatBlitEncoders,
                       kStatBarriers, kStatSyncs, kStatEventQueries, kStatPipelineAttempts, kStatGpuNanos, kStatGpuBusyNanos, kStatPassResumes, kStatPassResumesBarrier, kStatCount };
extern std::atomic<uint64_t> g_stats[kStatCount];
inline void stat_add(Stat stat) { g_stats[stat].fetch_add(1, std::memory_order_relaxed); }

struct Buffer;

// (type, pixel format, first mip, mips, first slice, slices, swizzle) of a texture view.
using ViewKey = std::array<uint32_t, 7>;
// Sampler description fields that matter to an MTLSamplerState (the LOD bias lives in the descriptor).
using SamplerKey = std::array<uint32_t, 11>;

// A converted shader stage: its Metal function and its reflection.
struct ShaderStage {
    id<MTLFunction> function = nil;
    // Stages converted for geometry/tessellation emulation keep their library: the runtime builds the functions
    // (with function constants) when it assembles the mesh pipeline.
    id<MTLLibrary> library = nil;
    std::string entry_name;
    // Emulation reflection (scalars of IRVSInfo / IRGSInfo / IRHSInfo / IRDSInfo).
    uint32_t vertex_output_size = 0;
    uint32_t gs_max_input_primitives = 0, gs_instance_count = 1, gs_input_primitive = 0;
    bool gs_passthrough = false;
    uint32_t hs_max_patches = 0, hs_max_object_threads = 0, hs_input_control_points = 0, hs_output_primitive = 0;
    float hs_max_tess_factor = 0;
    uint32_t ds_max_input_prims = 0;
    std::shared_ptr<IRShaderReflection> reflection;  // kept for stage-in synthesis
    uint32_t num_vertex_inputs = 0;
    uint32_t threadgroup_size[3] = {1, 1, 1};  // compute stages
    CacheKey cache_key{};                      // identifies the converted shader in the disk cache

    // Vertex stage-in functions synthesized for this shader, by serialized input layout.
    std::mutex stage_in_mutex;
    std::map<std::string, id<MTLFunction>> stage_ins;
    std::map<std::string, id<MTLLibrary>> emulation_stage_ins;  // the same for emulated pipelines
};

// (hash of the DXIL, DXIL size, root signature id, IRShaderStage, entry point)
using ShaderKey = std::tuple<uint64_t, uint64_t, uint64_t, uint32_t, std::string>;

// Depth/stencil description fields that matter to an MTLDepthStencilState.
using DepthStencilKey = std::array<uint32_t, 13>;

struct Device {
    id<MTLDevice> device;
    id<MTLResidencySet> residency;
    std::atomic<bool> residency_dirty{false};
    std::atomic<int> test_fail_pipeline{0};  // mtlb_device_test_fail_next_pipeline

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
    id<MTLLibrary> gdeflate_library = nil;  // the DirectStorage decompressor (gdeflate.mm), built on first use
    std::map<std::string, id<MTLComputePipelineState>> kernels;

    // Samplers shared by equal descriptions.
    std::mutex sampler_mutex;
    std::map<SamplerKey, id<MTLSamplerState>> samplers;

    // Buffers the emulated pipelines need bound whatever the application did (see queue.mm).
    std::mutex helper_mutex;
    id<MTLBuffer> zero_buffer = nil;   // stands in for descriptor heaps and root arguments that were not set
    id<MTLBuffer> tess_tables = nil;   // the tessellator lookup tables

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

constexpr uint32_t kQuerySlots = 8;
constexpr uint32_t kSamplesPerBuffer = 4096;  // 32 KB, the limit of a MTLCounterSampleBuffer

struct QueryHeap {
    Device *device;
    uint32_t kind;
    uint32_t count;
    id<MTLBuffer> results;                    // occlusion counts, 8 bytes per query
    // Timestamps (empty where the GPU cannot sample them): Metal limits a counter sample buffer to
    // kSamplesPerBuffer samples, so a large heap is several buffers, made when first used; query i is sample
    // i % kSamplesPerBuffer of buffer i / kSamplesPerBuffer. Use sample_buffer().
    std::vector<id<MTLCounterSampleBuffer>> samples;
    std::vector<uint8_t> samples_failed;  // per buffer: creation was tried and failed (not tried again)
    uint32_t sample_attempts = 0;         // buffer creations tried (mtlb_query_heap_test_sample_attempts)
    bool test_fail_samples = false;
    std::mutex samples_mutex;
    // Occlusion: a query that spans several render passes keeps one result slot per pass (kQuerySlots of them,
    // `results` holds count * kQuerySlots counts); resolving sums the slots used since the query began.
    std::vector<uint32_t> slots_used;
};

// The counter sample buffer of query `index` (nil when it cannot be made).
id<MTLCounterSampleBuffer> sample_buffer(QueryHeap *heap, uint32_t index);

struct Heap {
    Device *device;
    id<MTLHeap> heap;
    mtlb_storage storage;
    // A buffer over the whole heap, which is what the address table holds for it: buffers placed over each
    // other would overlap there. Absent for a heap larger than a buffer can be.
    Buffer *alias = nullptr;
};

struct Buffer {
    Buffer(Device *d, id<MTLBuffer> b, uint64_t address, uint64_t length)
        : device(d), buffer(b), gpu_address(address), size(length) {}

    Device *device;
    id<MTLBuffer> buffer;
    uint64_t gpu_address;
    uint64_t size;
    bool placed = false;  // lives in a heap, which owns its residency
    bool whole_heap = false;  // stands for a whole heap in the address table (never overlaps)
    bool registered = false;  // in the device's address table (see find_buffer)

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
    // (command buffers do not retain what they reference). Keys are mtlb formats, or the MTLPixelFormat with
    // kRawViewKey set (views of another type, see Pipeline::color_view_formats).
    std::mutex views_mutex;
    std::map<uint32_t, id<MTLTexture>> views;
    static constexpr uint32_t kRawViewKey = 0x80000000u;

    // Shader-visible views (type, range, format, swizzle), kept for the texture's life.
    std::map<ViewKey, id<MTLTexture>> sampled_views;
};

struct RootSignature {
    Device *device;
    uint64_t id;  // unique for the process lifetime, so cache keys never see a reused address
    IRRootSignature *ir;
    std::array<uint8_t, 32> blob_hash;  // SHA-256 of the serialized root signature the converter was given
};

// What the runtime needs to (re)build the mesh pipeline of a pipeline with geometry or tessellation stages.
struct EmulatedPipeline {
    id<MTLLibrary> stage_in = nil, vertex = nil, hull = nil, domain = nil, geometry = nil, fragment = nil;
    std::string vertex_name, geometry_name, fragment_name;
    bool tessellation = false;
    IRRuntimeGeometryPipelineConfig gs_config = {};
    IRRuntimeTessellationPipelineConfig ts_config = {};
    uint32_t patch_control_points = 0;  // tessellation: the hull shader's input control point count
};

// The mesh render pipeline of an emulated pipeline for `descriptor`'s attachments; nil with `error` set on failure.
id<MTLRenderPipelineState> build_emulated_state(Device *device, const EmulatedPipeline &emulated,
                                                MTLMeshRenderPipelineDescriptor *descriptor, NSError **error);

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
    // Geometry/tessellation emulation: a mesh pipeline instead (`descriptor` stays nil).
    std::unique_ptr<EmulatedPipeline> emulated;
    MTLMeshRenderPipelineDescriptor *mesh_descriptor = nil;
    MTLPixelFormat depth_format = MTLPixelFormatInvalid;
    MTLPixelFormat stencil_format = MTLPixelFormatInvalid;
    // Color targets the fragment shader writes with another type than the target's format (integers to a
    // normalised target, or floats to an integer one): the pipeline writes the target through a view of this
    // same-size format of the other type, and passes bind that view (Invalid: the target as it is).
    std::array<MTLPixelFormat, MTLB_MAX_RENDER_TARGETS> color_view_formats{};
    // The pipeline writes color or depth attachments: a draw needs them bound (without any, it runs in an attachment-less pass).
    bool has_attachments = false;
    // A stage binds a UAV: a pass that draws with it may have written resources a barrier orders (queue.mm).
    bool writes_uav = false;
    std::mutex variants_mutex;
    std::map<uint64_t, id<MTLRenderPipelineState>> variants;
    Device *device = nullptr;

    // The render pipeline state for a pass whose attachments have these pixel formats; nil with
    // fail() set when the variant cannot be built.
    id<MTLRenderPipelineState> state_for(MTLPixelFormat depth, MTLPixelFormat stencil);
};

// Private buffers waiting for reuse, the smallest that fits first.
struct BufferPool {
    std::mutex mutex;
    std::vector<id<MTLBuffer>> free;

    id<MTLBuffer> take(uint64_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto best = free.end();
        for (auto it = free.begin(); it != free.end(); ++it) {
            if ([*it length] >= size && (best == free.end() || [*it length] < [*best length]))
                best = it;
        }
        if (best == free.end())
            return nil;
        id<MTLBuffer> found = *best;
        free.erase(best);
        return found;
    }
    void give(const std::vector<id<MTLBuffer>> &buffers)
    {
        std::lock_guard<std::mutex> lock(mutex);
        free.insert(free.end(), buffers.begin(), buffers.end());
    }
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
    // Private buffers that command buffers have finished with: ExecuteIndirect scratch (untracked) and the staging of
    // copies between block sizes (tracked). Completion handlers hold the pools, so they outlive the queue.
    std::shared_ptr<BufferPool> scratch_pool = std::make_shared<BufferPool>();
    std::shared_ptr<BufferPool> staging_pool = std::make_shared<BufferPool>();

    id<MTLSharedEvent> resolve_event = nil;  // signalled by the completion handler that copies timestamps
    uint64_t resolve_value = 0, resolve_wait = 0;
    bool fence_pending = false;  // an encoder updated the fence and no later encoder has waited yet
    uint32_t test_drop_signals = 0;  // mtlb_queue_test_drop_signals
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

    // The performance overlay (hud=1): font and shader, a pipeline per drawable format.
    bool hud = false;
    HudState hud_state;
    id<MTLRenderPipelineState> hud_pipeline = nil;

    // Debug aid: D3D12METAL_DUMP_PRESENT=<file.png> writes what the Nth present
    // (D3D12METAL_DUMP_PRESENT_FRAME, default 30) drew onto the drawable.
    std::string dump_path;
    uint64_t dump_frame = 0;
    std::atomic<uint64_t> presents{0};
};

struct Event {
    Device *device;
    id<MTLSharedEvent> event;
    // The front-end's lock-free view of the event's value (mtlb_event_create); completion handlers keep it alive.
    std::shared_ptr<std::atomic<uint64_t>> mirror = std::make_shared<std::atomic<uint64_t>>(0);
};

// Makes `mirror` equal to the event's current value, whatever it is (Metal's events never go down: a signal with a lower
// value than the event has changes nothing, and the mirror has to say so). The value is read again after the store: a signal that landed in between would otherwise be overwritten by the
// older one for good.
inline void sync_mirror(std::atomic<uint64_t> &mirror, id<MTLSharedEvent> event)
{
    for (;;) {
        const uint64_t value = event.signaledValue;
        mirror.store(value, std::memory_order_release);
        if (event.signaledValue == value)
            return;
    }
}

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
    id<MTLRenderPipelineState> hud_pipeline = nil;  // when the overlay is on
};

// Takes the swap chain's next drawable and applies the sync interval. Blocks while
// every drawable is in flight, so call it without holding locks.
Drawable acquire_drawable(Swapchain *swapchain, uint32_t sync_interval);

// Encodes drawing `texture` onto the drawable and presenting it into `command_buffer`.
void encode_present(Queue *queue, id<MTLCommandBuffer> command_buffer, Swapchain *swapchain, Texture *texture,
                    const Drawable &drawable);

// The backend's own compute kernel `name` (kernels.mm); nil with fail() set when it cannot be built.
id<MTLComputePipelineState> internal_kernel(Device *device, NSString *name);
// The same for the GDeflate decompression kernels (gdeflate.mm).
id<MTLComputePipelineState> gdeflate_kernel(Device *device, NSString *name);

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
