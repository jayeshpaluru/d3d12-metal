/*
 * mtlb: the bridge between the D3D12/DXGI front-end and the Metal backend.
 *
 * Plain C, opaque 64-bit handles and POD structs only. No Metal or D3D types
 * cross this interface, so the same API can later be called through a Wine
 * unix-call thunk (every pointer argument is either an input blob or a small
 * out-struct, and every handle is a uint64_t).
 */
#ifndef MTLB_H
#define MTLB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTLB_EXPORT __attribute__((visibility("default")))

/* Compile-time layout checks. The structs below cross a process boundary later,
 * so their layout is part of the interface. */
#ifdef __cplusplus
#define MTLB_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define MTLB_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#define MTLB_ASSERT_SIZE(type, size) MTLB_STATIC_ASSERT(sizeof(type) == (size), #type " size changed")
#define MTLB_ASSERT_OFFSET(type, member, offset) \
    MTLB_STATIC_ASSERT(offsetof(type, member) == (offset), #type "." #member " offset changed")

/* ------------------------------------------------------------------------ */
/* Results and handles                                                      */
/* ------------------------------------------------------------------------ */

typedef int32_t mtlb_result;
enum {
    MTLB_OK = 0,
    MTLB_ERROR_INVALID_ARGUMENT = -1,
    MTLB_ERROR_OUT_OF_MEMORY = -2,
    MTLB_ERROR_UNSUPPORTED = -3,
    MTLB_ERROR_COMPILE_FAILED = -4,   /* shader conversion or pipeline build */
    MTLB_ERROR_TIMEOUT = -5,
    MTLB_ERROR_DEVICE = -6,           /* no Metal device or other Metal failure */
};

/* Description of the most recent failure on the calling thread. */
MTLB_EXPORT const char *mtlb_last_error(void);

/* Sets an option the backend reads from the environment (D3D12METAL_*) unless the environment already sets it:
 * the front-end passes the entries of d3d12metal.conf (Wine's unix side does not see the game's configuration). */
MTLB_EXPORT void mtlb_configure(const char *name, const char *value);

/* Log output. Channel 0 appends `length` bytes (whole lines) to D3D12METAL_LOG_FILE if it is set; channel 1
 * replaces the file D3D12METAL_LOG_FILE + ".methods" (the API trace's inventory of methods). */
MTLB_EXPORT void mtlb_log_write(uint32_t channel, const char *text, uint64_t length);

/* Opaque handles. Zero is never a valid handle. */
typedef uint64_t mtlb_device;
typedef uint64_t mtlb_buffer;
typedef uint64_t mtlb_texture;
typedef uint64_t mtlb_pipeline;
typedef uint64_t mtlb_root_signature;
typedef uint64_t mtlb_queue;
typedef uint64_t mtlb_event;
typedef uint64_t mtlb_notify;
typedef uint64_t mtlb_swapchain;
typedef uint64_t mtlb_heap;
typedef uint64_t mtlb_query_heap;

/* ------------------------------------------------------------------------ */
/* Formats                                                                  */
/* ------------------------------------------------------------------------ */

/* Enumerator values deliberately equal the DXGI_FORMAT values so the front-end
 * converts with a cast; the bridge does not otherwise depend on DXGI. */
typedef enum mtlb_format {
    MTLB_FORMAT_UNKNOWN = 0,
    MTLB_FORMAT_R32G32B32A32_TYPELESS = 1,
    MTLB_FORMAT_R32G32B32A32_FLOAT = 2,
    MTLB_FORMAT_R32G32B32A32_UINT = 3,
    MTLB_FORMAT_R32G32B32A32_SINT = 4,
    MTLB_FORMAT_R32G32B32_TYPELESS = 5,
    MTLB_FORMAT_R32G32B32_FLOAT = 6,
    MTLB_FORMAT_R32G32B32_UINT = 7,
    MTLB_FORMAT_R32G32B32_SINT = 8,
    MTLB_FORMAT_R16G16B16A16_TYPELESS = 9,
    MTLB_FORMAT_R16G16B16A16_FLOAT = 10,
    MTLB_FORMAT_R16G16B16A16_UNORM = 11,
    MTLB_FORMAT_R16G16B16A16_UINT = 12,
    MTLB_FORMAT_R16G16B16A16_SNORM = 13,
    MTLB_FORMAT_R16G16B16A16_SINT = 14,
    MTLB_FORMAT_R32G32_TYPELESS = 15,
    MTLB_FORMAT_R32G32_FLOAT = 16,
    MTLB_FORMAT_R32G32_UINT = 17,
    MTLB_FORMAT_R32G32_SINT = 18,
    MTLB_FORMAT_R32G8X24_TYPELESS = 19,
    MTLB_FORMAT_D32_FLOAT_S8X24_UINT = 20,
    MTLB_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
    MTLB_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
    MTLB_FORMAT_R10G10B10A2_TYPELESS = 23,
    MTLB_FORMAT_R10G10B10A2_UNORM = 24,
    MTLB_FORMAT_R10G10B10A2_UINT = 25,
    MTLB_FORMAT_R11G11B10_FLOAT = 26,
    MTLB_FORMAT_R8G8B8A8_TYPELESS = 27,
    MTLB_FORMAT_R8G8B8A8_UNORM = 28,
    MTLB_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    MTLB_FORMAT_R8G8B8A8_UINT = 30,
    MTLB_FORMAT_R8G8B8A8_SNORM = 31,
    MTLB_FORMAT_R8G8B8A8_SINT = 32,
    MTLB_FORMAT_R16G16_TYPELESS = 33,
    MTLB_FORMAT_R16G16_FLOAT = 34,
    MTLB_FORMAT_R16G16_UNORM = 35,
    MTLB_FORMAT_R16G16_UINT = 36,
    MTLB_FORMAT_R16G16_SNORM = 37,
    MTLB_FORMAT_R16G16_SINT = 38,
    MTLB_FORMAT_R32_TYPELESS = 39,
    MTLB_FORMAT_D32_FLOAT = 40,
    MTLB_FORMAT_R32_FLOAT = 41,
    MTLB_FORMAT_R32_UINT = 42,
    MTLB_FORMAT_R32_SINT = 43,
    MTLB_FORMAT_R24G8_TYPELESS = 44,
    MTLB_FORMAT_D24_UNORM_S8_UINT = 45,
    MTLB_FORMAT_R24_UNORM_X8_TYPELESS = 46,
    MTLB_FORMAT_X24_TYPELESS_G8_UINT = 47,
    MTLB_FORMAT_R8G8_TYPELESS = 48,
    MTLB_FORMAT_R8G8_UNORM = 49,
    MTLB_FORMAT_R8G8_UINT = 50,
    MTLB_FORMAT_R8G8_SNORM = 51,
    MTLB_FORMAT_R8G8_SINT = 52,
    MTLB_FORMAT_R16_TYPELESS = 53,
    MTLB_FORMAT_R16_FLOAT = 54,
    MTLB_FORMAT_D16_UNORM = 55,
    MTLB_FORMAT_R16_UNORM = 56,
    MTLB_FORMAT_R16_UINT = 57,
    MTLB_FORMAT_R16_SNORM = 58,
    MTLB_FORMAT_R16_SINT = 59,
    MTLB_FORMAT_R8_TYPELESS = 60,
    MTLB_FORMAT_R8_UNORM = 61,
    MTLB_FORMAT_R8_UINT = 62,
    MTLB_FORMAT_R8_SNORM = 63,
    MTLB_FORMAT_R8_SINT = 64,
    MTLB_FORMAT_A8_UNORM = 65,
    MTLB_FORMAT_R9G9B9E5_SHAREDEXP = 67,
    MTLB_FORMAT_BC1_TYPELESS = 70,
    MTLB_FORMAT_BC1_UNORM = 71,
    MTLB_FORMAT_BC1_UNORM_SRGB = 72,
    MTLB_FORMAT_BC2_TYPELESS = 73,
    MTLB_FORMAT_BC2_UNORM = 74,
    MTLB_FORMAT_BC2_UNORM_SRGB = 75,
    MTLB_FORMAT_BC3_TYPELESS = 76,
    MTLB_FORMAT_BC3_UNORM = 77,
    MTLB_FORMAT_BC3_UNORM_SRGB = 78,
    MTLB_FORMAT_BC4_TYPELESS = 79,
    MTLB_FORMAT_BC4_UNORM = 80,
    MTLB_FORMAT_BC4_SNORM = 81,
    MTLB_FORMAT_BC5_TYPELESS = 82,
    MTLB_FORMAT_BC5_UNORM = 83,
    MTLB_FORMAT_BC5_SNORM = 84,
    MTLB_FORMAT_B5G6R5_UNORM = 85,
    MTLB_FORMAT_B5G5R5A1_UNORM = 86,
    MTLB_FORMAT_B8G8R8A8_UNORM = 87,
    MTLB_FORMAT_B8G8R8A8_TYPELESS = 90,
    MTLB_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    MTLB_FORMAT_BC6H_TYPELESS = 94,
    MTLB_FORMAT_BC6H_UF16 = 95,
    MTLB_FORMAT_BC6H_SF16 = 96,
    MTLB_FORMAT_BC7_TYPELESS = 97,
    MTLB_FORMAT_BC7_UNORM = 98,
    MTLB_FORMAT_BC7_UNORM_SRGB = 99,
} mtlb_format;

enum {
    MTLB_FORMAT_FLAG_COLOR = 1u << 0,
    MTLB_FORMAT_FLAG_DEPTH = 1u << 1,
    MTLB_FORMAT_FLAG_STENCIL = 1u << 2,
    MTLB_FORMAT_FLAG_COMPRESSED = 1u << 3,
    MTLB_FORMAT_FLAG_TEXTURE = 1u << 4,        /* usable as a sampled texture */
    MTLB_FORMAT_FLAG_RENDER_TARGET = 1u << 5,
    MTLB_FORMAT_FLAG_BLENDABLE = 1u << 6,
    MTLB_FORMAT_FLAG_VERTEX = 1u << 7,         /* usable as a vertex attribute */
    MTLB_FORMAT_FLAG_SRGB = 1u << 8,
    MTLB_FORMAT_FLAG_TYPELESS = 1u << 9,
    MTLB_FORMAT_FLAG_SHADER_WRITE = 1u << 10,  /* usable as a UAV */
    MTLB_FORMAT_FLAG_BUFFER = 1u << 11,        /* usable as a typed buffer view */
};

typedef struct mtlb_format_info {
    uint32_t block_width;      /* texels per block, 1 for uncompressed */
    uint32_t block_height;
    uint32_t bytes_per_block;  /* bytes per texel for uncompressed formats */
    uint32_t flags;            /* MTLB_FORMAT_FLAG_* */
} mtlb_format_info;

/* Pure lookup, no device needed. Returns MTLB_ERROR_UNSUPPORTED for formats the
 * bridge does not know. */
MTLB_EXPORT mtlb_result mtlb_format_get_info(mtlb_format format, mtlb_format_info *out);

/* ------------------------------------------------------------------------ */
/* Device                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct mtlb_device_caps {
    char name[256];
    uint64_t registry_id;
    uint64_t recommended_max_working_set_size;
    uint64_t max_buffer_length;
    uint32_t has_unified_memory;
    uint32_t sample_counts;    /* bit n set: textures with n samples are supported */
    uint64_t max_texture_buffer_width;  /* texels of a typed buffer view (texture buffer) */
    uint64_t current_allocated_size;    /* bytes of resources the process has allocated on the device */
} mtlb_device_caps;

/* Opens the Metal device with the given registry id (mtlb_device_caps::registry_id),
 * or the system default device when `registry_id` is 0. */
MTLB_EXPORT mtlb_result mtlb_device_create(uint64_t registry_id, mtlb_device *out);
MTLB_EXPORT void mtlb_device_destroy(mtlb_device device);

/* Description of a Metal device without opening it (no residency set, listener
 * or queue). `registry_id` 0 selects the system default device. */
MTLB_EXPORT mtlb_result mtlb_query_caps(uint64_t registry_id, mtlb_device_caps *out);
/* Description of the `index`th Metal device, in system order; fails with
 * MTLB_ERROR_INVALID_ARGUMENT past the last one. */
MTLB_EXPORT mtlb_result mtlb_enum_devices(uint32_t index, mtlb_device_caps *out);
MTLB_EXPORT mtlb_result mtlb_device_get_caps(mtlb_device device, mtlb_device_caps *out);

/* The on-disk cache of converted shaders. It lives in ~/Library/Caches/d3d12metal/<app_name>
 * (D3D12METAL_CACHE_DIR replaces the whole path, D3D12METAL_CACHE=0 turns it off,
 * D3D12METAL_CACHE_MAX_MB sets the size limit, 1024 by default). `app_name` is the
 * executable's name; call this before the first pipeline is created (later calls are ignored). */
MTLB_EXPORT void mtlb_cache_configure(const char *app_name);

/* Backend counters since the process started (cheap relaxed atomics, always on); the front-end prints
 * per-frame averages when D3D12METAL_STATS=1. */
typedef struct mtlb_stats {
    uint64_t submits;          /* mtlb_queue_submit calls */
    uint64_t command_buffers;  /* committed MTLCommandBuffers */
    uint64_t render_encoders;  /* render passes */
    uint64_t compute_encoders;
    uint64_t blit_encoders;
    uint64_t barriers;         /* barrier records that ended encoders / ordered work */
    uint64_t syncs;            /* encoders that waited on the queue fence */
    uint64_t event_queries;    /* mtlb_event_completed_value calls (fence reads that went to Metal) */
    uint64_t pipeline_attempts; /* mtlb_pipeline_create and mtlb_compute_pipeline_create calls */
    uint64_t gpu_nanos;        /* GPU time of completed command buffers (GPUEndTime - GPUStartTime, summed) */
} mtlb_stats;
MTLB_EXPORT void mtlb_stats_get(mtlb_stats *out);

typedef struct mtlb_cache_stats {
    uint64_t hits, misses;     /* lookups: found, not found */
    uint64_t writes;           /* entries stored */
    uint64_t corrupt;          /* entries rejected (damaged, wrong version) and rebuilt */
    uint64_t evicted;          /* files deleted to stay under the size limit */
    uint64_t bytes_on_disk;    /* current size of the cache directory */
} mtlb_cache_stats;
MTLB_EXPORT void mtlb_cache_get_stats(mtlb_cache_stats *out);

/* ------------------------------------------------------------------------ */
/* Buffers                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum mtlb_storage {
    MTLB_STORAGE_SHARED = 0,   /* CPU-visible */
    MTLB_STORAGE_PRIVATE = 1,  /* GPU only */
} mtlb_storage;

typedef struct mtlb_buffer_info {
    void *cpu_ptr;             /* NULL for private storage */
    uint64_t gpu_address;
    uint64_t size;
} mtlb_buffer_info;

/* Buffers are zero-initialised. */
MTLB_EXPORT mtlb_result mtlb_buffer_create(mtlb_device device, uint64_t size, mtlb_storage storage,
                                           mtlb_buffer *out, mtlb_buffer_info *info);
MTLB_EXPORT void mtlb_buffer_destroy(mtlb_buffer buffer);

/* ------------------------------------------------------------------------ */
/* Descriptor heaps                                                         */
/* ------------------------------------------------------------------------ */

/* One slot of a shader-visible descriptor heap. Layout-identical to the Metal
 * shader converter's IRDescriptorTableEntry (checked by the backend). */
typedef struct mtlb_descriptor {
    uint64_t gpu_address;      /* buffer address */
    uint64_t texture_id;       /* texture resource id */
    uint64_t metadata;         /* buffer length (low 32 bits) or texture min LOD */
} mtlb_descriptor;

static inline void mtlb_descriptor_set_buffer(mtlb_descriptor *d, uint64_t gpu_address, uint32_t size_bytes)
{
    d->gpu_address = gpu_address;
    d->texture_id = 0;
    d->metadata = size_bytes;
}

/* Creates a zeroed, CPU-visible buffer of `count` mtlb_descriptor entries.
 * info->gpu_address is the GPU descriptor handle base. */
MTLB_EXPORT mtlb_result mtlb_descriptor_heap_create(mtlb_device device, uint32_t count,
                                                    mtlb_buffer *out, mtlb_buffer_info *info);

/* ------------------------------------------------------------------------ */
/* Textures                                                                 */
/* ------------------------------------------------------------------------ */

typedef enum mtlb_texture_dimension {
    MTLB_TEXTURE_1D = 1,
    MTLB_TEXTURE_2D = 2,
    MTLB_TEXTURE_3D = 3,
} mtlb_texture_dimension;

enum {
    MTLB_TEXTURE_USAGE_SHADER_READ = 1u << 0,
    MTLB_TEXTURE_USAGE_SHADER_WRITE = 1u << 1,
    MTLB_TEXTURE_USAGE_RENDER_TARGET = 1u << 2,  /* colour or depth/stencil attachment */
    MTLB_TEXTURE_USAGE_DEPTH_STENCIL = 1u << 3,  /* with RENDER_TARGET: picks the depth variant of a typeless format */
};

typedef struct mtlb_texture_desc {
    uint32_t dimension;        /* mtlb_texture_dimension */
    uint32_t format;           /* mtlb_format */
    uint32_t width;
    uint32_t height;
    uint32_t depth_or_array_size;
    uint32_t mip_levels;
    uint32_t sample_count;
    uint32_t usage;            /* MTLB_TEXTURE_USAGE_* */
    uint32_t storage;          /* mtlb_storage */
    uint32_t reserved;
} mtlb_texture_desc;

typedef struct mtlb_texture_info {
    uint64_t resource_id;      /* value stored in mtlb_descriptor::texture_id */
} mtlb_texture_info;

MTLB_EXPORT mtlb_result mtlb_texture_create(mtlb_device device, const mtlb_texture_desc *desc,
                                            mtlb_texture *out, mtlb_texture_info *info);
MTLB_EXPORT void mtlb_texture_destroy(mtlb_texture texture);

/* ------------------------------------------------------------------------ */
/* Placement heaps                                                          */
/* ------------------------------------------------------------------------ */

typedef struct mtlb_size_align {
    uint64_t size;
    uint64_t align;
} mtlb_size_align;

/* What a buffer or texture takes in a heap (the descriptor must be the one it will be created with). */
MTLB_EXPORT mtlb_result mtlb_buffer_size_align(mtlb_device device, uint64_t size, mtlb_storage storage,
                                               mtlb_size_align *out);
MTLB_EXPORT mtlb_result mtlb_texture_size_align(mtlb_device device, const mtlb_texture_desc *desc,
                                                mtlb_size_align *out);

/* A placement heap: resources are created at offsets of it and may overlap (alias). Its memory is
 * resident for the device's queues for as long as it exists. */
MTLB_EXPORT mtlb_result mtlb_heap_create(mtlb_device device, uint64_t size, mtlb_storage storage, mtlb_heap *out);
MTLB_EXPORT void mtlb_heap_destroy(mtlb_heap heap);

/* Resources placed in a heap; the heap must outlive them. Their contents are whatever the memory held. */
MTLB_EXPORT mtlb_result mtlb_buffer_create_in_heap(mtlb_heap heap, uint64_t offset, uint64_t size, mtlb_buffer *out,
                                                   mtlb_buffer_info *info);
MTLB_EXPORT mtlb_result mtlb_texture_create_in_heap(mtlb_heap heap, uint64_t offset, const mtlb_texture_desc *desc,
                                                    mtlb_texture *out, mtlb_texture_info *info);

/* ------------------------------------------------------------------------ */
/* Descriptor contents                                                      */
/* ------------------------------------------------------------------------ */

/* The shape a shader sees a texture in. 1D textures are 2D textures of height 1. */
typedef enum mtlb_view_type {
    MTLB_VIEW_2D = 1,
    MTLB_VIEW_2D_ARRAY,
    MTLB_VIEW_2D_MS,
    MTLB_VIEW_2D_MS_ARRAY,
    MTLB_VIEW_3D,
    MTLB_VIEW_CUBE,
    MTLB_VIEW_CUBE_ARRAY,
} mtlb_view_type;

typedef struct mtlb_texture_view_desc {
    uint32_t type;             /* mtlb_view_type */
    uint32_t format;           /* mtlb_format to view the texture as; 0 = its own */
    uint32_t first_mip, mip_count;
    uint32_t first_slice;      /* array slice range; for cube views the count is faces (6 per cube) */
    uint32_t slice_count;
    uint32_t component_mapping; /* D3D12 Shader4ComponentMapping; 0 = identity */
    uint32_t reserved;
} mtlb_texture_view_desc;

/* The resource id to put in mtlb_descriptor::texture_id for a view of `texture`
 * (the texture itself when the view changes nothing). Views are cached on the
 * texture and live as long as it does. */
MTLB_EXPORT mtlb_result mtlb_texture_view(mtlb_texture texture, const mtlb_texture_view_desc *desc,
                                          uint64_t *resource_id);

typedef struct mtlb_buffer_view_desc {
    mtlb_buffer buffer;
    uint64_t offset;           /* bytes */
    uint64_t size;             /* bytes covered by the view */
    uint32_t format;           /* typed views: mtlb_format of an element; 0 = raw or structured */
    uint32_t num_elements;     /* typed views */
    mtlb_buffer counter_buffer;  /* UAV counter, or 0 */
    uint64_t counter_offset;   /* bytes, 4-aligned */
} mtlb_buffer_view_desc;

/* Fills `out` for a view of a buffer. A typed view whose element count exceeds the
 * device's texture buffer width is clamped to it (logged once); `out->metadata`
 * keeps the view's byte size. */
MTLB_EXPORT mtlb_result mtlb_buffer_view(const mtlb_buffer_view_desc *desc, mtlb_descriptor *out);

typedef enum mtlb_null_kind {
    MTLB_NULL_TEXTURE_2D = 1,
    MTLB_NULL_TEXTURE_2D_ARRAY,
    MTLB_NULL_TEXTURE_2D_MS,
    MTLB_NULL_TEXTURE_2D_MS_ARRAY,
    MTLB_NULL_TEXTURE_3D,
    MTLB_NULL_TEXTURE_CUBE,
    MTLB_NULL_TEXTURE_CUBE_ARRAY,
    MTLB_NULL_TYPED_BUFFER,
    MTLB_NULL_BUFFER,
    MTLB_NULL_UAV_TEXTURE_2D,
    MTLB_NULL_UAV_TEXTURE_2D_ARRAY,
    MTLB_NULL_UAV_TEXTURE_3D,
    MTLB_NULL_UAV_TYPED_BUFFER,
} mtlb_null_kind;

/* A descriptor that reads as zero (and swallows writes) for a null view. */
MTLB_EXPORT mtlb_result mtlb_null_descriptor(mtlb_device device, uint32_t kind, mtlb_descriptor *out);

/* Values equal the D3D12 enumerators. */
typedef struct mtlb_sampler_desc {
    uint32_t min_filter;       /* 0 point, 1 linear */
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t address_u, address_v, address_w;  /* D3D12_TEXTURE_ADDRESS_MODE: 1 wrap .. 5 mirror once */
    float mip_lod_bias;
    uint32_t max_anisotropy;   /* 1 = off */
    uint32_t compare_func;     /* mtlb_compare, 0 = no comparison */
    uint32_t reduction;        /* 0 = weighted average; 1 min, 2 max (not supported: logged, averaged) */
    float border_color[4];
    float min_lod, max_lod;
} mtlb_sampler_desc;

/* Fills `out` (a sampler heap entry). Equal samplers share one Metal sampler. */
MTLB_EXPORT mtlb_result mtlb_sampler_create(mtlb_device device, const mtlb_sampler_desc *desc, mtlb_descriptor *out);

/* ------------------------------------------------------------------------ */
/* Pipelines                                                                */
/* ------------------------------------------------------------------------ */

/* Enumerator values below equal the corresponding D3D12 enum values. */

typedef enum mtlb_blend {
    MTLB_BLEND_ZERO = 1, MTLB_BLEND_ONE = 2, MTLB_BLEND_SRC_COLOR = 3, MTLB_BLEND_INV_SRC_COLOR = 4,
    MTLB_BLEND_SRC_ALPHA = 5, MTLB_BLEND_INV_SRC_ALPHA = 6, MTLB_BLEND_DEST_ALPHA = 7,
    MTLB_BLEND_INV_DEST_ALPHA = 8, MTLB_BLEND_DEST_COLOR = 9, MTLB_BLEND_INV_DEST_COLOR = 10,
    MTLB_BLEND_SRC_ALPHA_SAT = 11, MTLB_BLEND_BLEND_FACTOR = 14, MTLB_BLEND_INV_BLEND_FACTOR = 15,
    MTLB_BLEND_SRC1_COLOR = 16, MTLB_BLEND_INV_SRC1_COLOR = 17, MTLB_BLEND_SRC1_ALPHA = 18,
    MTLB_BLEND_INV_SRC1_ALPHA = 19,
} mtlb_blend;

typedef enum mtlb_blend_op {
    MTLB_BLEND_OP_ADD = 1, MTLB_BLEND_OP_SUBTRACT = 2, MTLB_BLEND_OP_REV_SUBTRACT = 3,
    MTLB_BLEND_OP_MIN = 4, MTLB_BLEND_OP_MAX = 5,
} mtlb_blend_op;

typedef enum mtlb_compare {
    MTLB_COMPARE_NEVER = 1, MTLB_COMPARE_LESS = 2, MTLB_COMPARE_EQUAL = 3, MTLB_COMPARE_LESS_EQUAL = 4,
    MTLB_COMPARE_GREATER = 5, MTLB_COMPARE_NOT_EQUAL = 6, MTLB_COMPARE_GREATER_EQUAL = 7,
    MTLB_COMPARE_ALWAYS = 8,
} mtlb_compare;

typedef enum mtlb_stencil_op {
    MTLB_STENCIL_OP_KEEP = 1, MTLB_STENCIL_OP_ZERO = 2, MTLB_STENCIL_OP_REPLACE = 3,
    MTLB_STENCIL_OP_INCR_SAT = 4, MTLB_STENCIL_OP_DECR_SAT = 5, MTLB_STENCIL_OP_INVERT = 6,
    MTLB_STENCIL_OP_INCR = 7, MTLB_STENCIL_OP_DECR = 8,
} mtlb_stencil_op;

typedef enum mtlb_cull_mode { MTLB_CULL_NONE = 1, MTLB_CULL_FRONT = 2, MTLB_CULL_BACK = 3 } mtlb_cull_mode;
typedef enum mtlb_fill_mode { MTLB_FILL_WIREFRAME = 2, MTLB_FILL_SOLID = 3 } mtlb_fill_mode;

typedef enum mtlb_topology_type {
    MTLB_TOPOLOGY_TYPE_POINT = 1, MTLB_TOPOLOGY_TYPE_LINE = 2, MTLB_TOPOLOGY_TYPE_TRIANGLE = 3,
} mtlb_topology_type;

typedef enum mtlb_input_class { MTLB_INPUT_PER_VERTEX = 0, MTLB_INPUT_PER_INSTANCE = 1 } mtlb_input_class;

#define MTLB_MAX_RENDER_TARGETS 8
#define MTLB_MAX_INPUT_ELEMENTS 31
#define MTLB_MAX_VERTEX_BUFFERS 31
#define MTLB_MAX_VIEWPORTS 16     /* also the number of scissor rectangles */

typedef struct mtlb_input_element {
    char semantic_name[32];
    uint32_t semantic_index;
    uint32_t format;           /* mtlb_format, must have MTLB_FORMAT_FLAG_VERTEX */
    uint32_t input_slot;
    uint32_t byte_offset;
    uint32_t input_class;      /* mtlb_input_class */
    uint32_t step_rate;
} mtlb_input_element;

typedef struct mtlb_stencil_face {
    uint32_t fail_op, depth_fail_op, pass_op;  /* mtlb_stencil_op */
    uint32_t func;                             /* mtlb_compare */
} mtlb_stencil_face;

typedef struct mtlb_render_target_blend {
    uint32_t blend_enable;
    uint32_t src_blend, dest_blend, blend_op;              /* colour */
    uint32_t src_blend_alpha, dest_blend_alpha, blend_op_alpha;
    uint32_t write_mask;                                   /* bit0=R .. bit3=A */
} mtlb_render_target_blend;

#define MTLB_MAX_ROOT_PARAMETERS 64

/* Where one root parameter lives in the top-level argument buffer. */
typedef struct mtlb_root_parameter_layout {
    uint32_t offset;           /* bytes from the start of the argument buffer */
    uint32_t size;             /* bytes: 4 * Num32BitValues for constants, 8 otherwise */
} mtlb_root_parameter_layout;

typedef struct mtlb_root_signature_layout {
    uint32_t num_parameters;
    uint32_t argument_buffer_size;   /* bytes, multiple of 8 */
    mtlb_root_parameter_layout parameters[MTLB_MAX_ROOT_PARAMETERS];
} mtlb_root_signature_layout;

/* Builds the shader converter's root signature once from a serialized root
 * signature (DXBC container or bare RTS0 blob) and reports the top-level
 * argument buffer layout it implies. Pipelines are built against the handle. */
MTLB_EXPORT mtlb_result mtlb_root_signature_create(mtlb_device device, const void *blob, uint64_t size,
                                                   mtlb_root_signature *out, mtlb_root_signature_layout *layout);
MTLB_EXPORT void mtlb_root_signature_destroy(mtlb_root_signature root_signature);

typedef struct mtlb_pipeline_desc {
    /* DXIL blobs; a missing stage has size 0. */
    const void *vs_dxil; uint64_t vs_size;
    const void *ps_dxil; uint64_t ps_size;
    const char *vs_entry;      /* NULL: the entry point named in the DXIL */
    const char *ps_entry;
    mtlb_root_signature root_signature;

    uint32_t num_render_targets;
    uint32_t rtv_formats[MTLB_MAX_RENDER_TARGETS];  /* mtlb_format */
    uint32_t dsv_format;
    uint32_t sample_count;
    uint32_t topology_type;    /* mtlb_topology_type */

    uint32_t independent_blend;
    mtlb_render_target_blend blend[MTLB_MAX_RENDER_TARGETS];

    uint32_t cull_mode;        /* mtlb_cull_mode */
    uint32_t front_counter_clockwise;
    uint32_t fill_mode;        /* mtlb_fill_mode */
    int32_t depth_bias;
    float depth_bias_clamp;
    float slope_scaled_depth_bias;
    uint32_t depth_clip_enable;

    uint32_t depth_enable;
    uint32_t depth_write_enable;
    uint32_t depth_func;       /* mtlb_compare */
    uint32_t stencil_enable;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    mtlb_stencil_face front_face, back_face;

    uint32_t num_input_elements;
    mtlb_input_element input_elements[MTLB_MAX_INPUT_ELEMENTS];
} mtlb_pipeline_desc;

typedef struct mtlb_compute_pipeline_desc {
    const void *cs_dxil; uint64_t cs_size;
    const char *cs_entry;      /* NULL: the entry point named in the DXIL */
    mtlb_root_signature root_signature;
} mtlb_compute_pipeline_desc;

/* Converts the compute shader and builds the compute pipeline. The handle is an
 * mtlb_pipeline like the graphics ones (destroy with mtlb_pipeline_destroy); the
 * command stream tells the two kinds apart. */
MTLB_EXPORT mtlb_result mtlb_compute_pipeline_create(mtlb_device device, const mtlb_compute_pipeline_desc *desc,
                                                     mtlb_pipeline *out);

/* Converts the DXIL with the Metal shader converter against the root signature
 * and builds the render pipeline and depth-stencil state. */
MTLB_EXPORT mtlb_result mtlb_pipeline_create(mtlb_device device, const mtlb_pipeline_desc *desc,
                                             mtlb_pipeline *out);
MTLB_EXPORT void mtlb_pipeline_destroy(mtlb_pipeline pipeline);

/* ------------------------------------------------------------------------ */
/* Queues and events                                                        */
/* ------------------------------------------------------------------------ */

MTLB_EXPORT mtlb_result mtlb_queue_create(mtlb_device device, mtlb_queue *out);
MTLB_EXPORT void mtlb_queue_destroy(mtlb_queue queue);

/* A contiguous byte range; one command stream per span. */
typedef struct mtlb_span {
    const uint8_t *data;
    uint64_t size;
} mtlb_span;

/* Replays `count` command streams (see mtlb_cmd.h), in order, into one command
 * buffer and commits it. Each stream starts with RESET_STATE, so draw state does
 * not carry from one span to the next. Returns once the work is committed, not
 * completed. */
MTLB_EXPORT mtlb_result mtlb_queue_submit(mtlb_queue queue, const mtlb_span *spans, uint32_t count);

/* Number of render passes (render command encoders) the queue has encoded so
 * far; for tests and diagnostics. */
MTLB_EXPORT uint64_t mtlb_queue_render_pass_count(mtlb_queue queue);

/* GPU-timeline signal and wait, ordered with respect to prior submissions. */
MTLB_EXPORT mtlb_result mtlb_queue_signal(mtlb_queue queue, mtlb_event event, uint64_t value);
MTLB_EXPORT mtlb_result mtlb_queue_wait(mtlb_queue queue, mtlb_event event, uint64_t value);

/* `mirror` receives the address of a 64-bit value the backend keeps equal to the event's completed value: updated
 * (atomically, monotonically for GPU signals) when a queue signal completes and by mtlb_event_signal_cpu. Read it with
 * an atomic load from the same address space instead of calling mtlb_event_completed_value, which is a bridge
 * call. It may lag the event slightly; it is valid until mtlb_event_destroy. */
MTLB_EXPORT mtlb_result mtlb_event_create(mtlb_device device, uint64_t initial_value, mtlb_event *out,
                                          uint64_t **mirror);
MTLB_EXPORT void mtlb_event_destroy(mtlb_event event);
/* The event's value as Metal has it (the mirror may lag it). */
MTLB_EXPORT uint64_t mtlb_event_completed_value(mtlb_event event);
MTLB_EXPORT void mtlb_event_signal_cpu(mtlb_event event, uint64_t value);
/* Blocks until the event reaches `value`; timeout_ms of UINT64_MAX waits forever.
 * Returns MTLB_ERROR_TIMEOUT on timeout. */
MTLB_EXPORT mtlb_result mtlb_event_wait_cpu(mtlb_event event, uint64_t value, uint64_t timeout_ms);

/* ------------------------------------------------------------------------ */
/* Swap chains                                                              */
/* ------------------------------------------------------------------------ */

typedef struct mtlb_swapchain_desc {
    uint64_t window;           /* top-level window handle (a Wine HWND) */
    uint32_t width;            /* drawable size in pixels */
    uint32_t height;
    uint32_t format;           /* mtlb_format of the application's back buffers */
    uint32_t buffer_count;     /* drawables the layer may hand out at once (2 or 3) */
} mtlb_swapchain_desc;

/* Attaches a Metal layer to the window and configures it for `desc`. Fails with
 * MTLB_ERROR_UNSUPPORTED where windows cannot be resolved to a layer (the native
 * headless build) and with MTLB_ERROR_DEVICE when the window has none. */
MTLB_EXPORT mtlb_result mtlb_swapchain_create(mtlb_device device, const mtlb_swapchain_desc *desc,
                                              mtlb_swapchain *out);
MTLB_EXPORT void mtlb_swapchain_destroy(mtlb_swapchain swapchain);
/* Changes drawable size and format (the buffer count stays). */
MTLB_EXPORT mtlb_result mtlb_swapchain_resize(mtlb_swapchain swapchain, uint32_t width, uint32_t height,
                                              uint32_t format);

/* Encodes the presentation of `texture` (a back buffer of the swap chain's size
 * or any other, it is scaled; any format the swap chain accepts, it is
 * converted) after the queue's earlier work, then commits. `sync_interval` 0
 * presents without waiting for the display. */
MTLB_EXPORT mtlb_result mtlb_queue_present(mtlb_queue queue, mtlb_swapchain swapchain, mtlb_texture texture,
                                           uint32_t sync_interval);

/* ------------------------------------------------------------------------ */
/* Queries                                                                  */
/* ------------------------------------------------------------------------ */

/* Query kinds and types; the values equal D3D12_QUERY_TYPE. A heap of kind OCCLUSION serves both occlusion
 * query types. */
enum {
    MTLB_QUERY_OCCLUSION = 0,
    MTLB_QUERY_BINARY_OCCLUSION = 1,
    MTLB_QUERY_TIMESTAMP = 2,
    MTLB_QUERY_PIPELINE_STATISTICS = 3,
    MTLB_QUERY_SO_STATISTICS = 4,
};

MTLB_EXPORT mtlb_result mtlb_query_heap_create(mtlb_device device, uint32_t kind, uint32_t count, mtlb_query_heap *out);
MTLB_EXPORT void mtlb_query_heap_destroy(mtlb_query_heap heap);

/* GPU timestamps tick at this rate. */
MTLB_EXPORT uint64_t mtlb_timestamp_frequency(mtlb_device device);
/* The GPU clock and the CPU clock (mach_absolute_time ticks) sampled together. */
MTLB_EXPORT mtlb_result mtlb_gpu_clock(mtlb_device device, uint64_t *gpu_ticks, uint64_t *cpu_ticks);

/* Debug groups on the queue's open command buffer (kind: 0 push, 1 pop; a signpost is a push and a pop). */
MTLB_EXPORT mtlb_result mtlb_queue_marker(mtlb_queue queue, uint32_t kind, const char *text);

/* ------------------------------------------------------------------------ */
/* Completion notifications                                                 */
/* ------------------------------------------------------------------------ */

/* A notification queue collects "event reached value" notifications. Metal
 * delivers them on its own threads, which must not run front-end code (under
 * Wine they have no Windows thread state), so the backend only records them;
 * one front-end thread blocks in mtlb_notify_wait and acts on them. */
typedef struct mtlb_notification {
    uint64_t cookie;           /* the value given to mtlb_event_notify */
    uint64_t value;            /* the value that was reached */
} mtlb_notification;

MTLB_EXPORT mtlb_result mtlb_notify_create(mtlb_notify *out);
/* Closes the queue (waking any waiter) and releases the caller's handle.
 * Registrations still pending in Metal are dropped safely. */
MTLB_EXPORT void mtlb_notify_destroy(mtlb_notify queue);

/* Queues {cookie, value} once `event` reaches `value` (at once if it already has). */
MTLB_EXPORT mtlb_result mtlb_event_notify(mtlb_event event, uint64_t value, mtlb_notify queue, uint64_t cookie);

/* Blocks (no timeout) until a notification is queued or the queue is closed,
 * then moves up to `max` notifications to `out`. *count is 0 when closed. */
MTLB_EXPORT mtlb_result mtlb_notify_wait(mtlb_notify queue, mtlb_notification *out, uint32_t max, uint32_t *count);
/* Makes the current and every later mtlb_notify_wait return 0 notifications. */
MTLB_EXPORT void mtlb_notify_close(mtlb_notify queue);

MTLB_ASSERT_SIZE(mtlb_notification, 16);
MTLB_ASSERT_SIZE(mtlb_span, 16);
MTLB_ASSERT_OFFSET(mtlb_span, size, 8);
MTLB_ASSERT_SIZE(mtlb_format_info, 16);
MTLB_ASSERT_SIZE(mtlb_device_caps, 304);
MTLB_ASSERT_SIZE(mtlb_cache_stats, 48);
MTLB_ASSERT_SIZE(mtlb_stats, 80);
MTLB_ASSERT_SIZE(mtlb_buffer_info, 24);
MTLB_ASSERT_OFFSET(mtlb_buffer_info, gpu_address, 8);
MTLB_ASSERT_SIZE(mtlb_descriptor, 24);
MTLB_ASSERT_OFFSET(mtlb_descriptor, texture_id, 8);
MTLB_ASSERT_OFFSET(mtlb_descriptor, metadata, 16);
MTLB_ASSERT_SIZE(mtlb_texture_desc, 40);
MTLB_ASSERT_SIZE(mtlb_texture_info, 8);
MTLB_ASSERT_SIZE(mtlb_input_element, 56);
MTLB_ASSERT_SIZE(mtlb_stencil_face, 16);
MTLB_ASSERT_SIZE(mtlb_render_target_blend, 32);
MTLB_ASSERT_SIZE(mtlb_root_parameter_layout, 8);
MTLB_ASSERT_SIZE(mtlb_root_signature_layout, 520);
MTLB_ASSERT_SIZE(mtlb_pipeline_desc, 2192);
MTLB_ASSERT_SIZE(mtlb_compute_pipeline_desc, 32);
MTLB_ASSERT_SIZE(mtlb_swapchain_desc, 24);
MTLB_ASSERT_SIZE(mtlb_size_align, 16);
MTLB_ASSERT_SIZE(mtlb_texture_view_desc, 32);
MTLB_ASSERT_SIZE(mtlb_buffer_view_desc, 48);
MTLB_ASSERT_SIZE(mtlb_sampler_desc, 64);

#ifdef __cplusplus
}
#endif

#endif /* MTLB_H */
