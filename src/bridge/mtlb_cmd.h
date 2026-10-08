/*
 * mtlb command stream.
 *
 * The front-end records D3D12 command lists into a flat byte stream of tagged
 * POD records; mtlb_queue_submit() replays one stream into one MTLCommandBuffer.
 *
 * Every record starts with mtlb_cmd_header. `size` is the full record size in
 * bytes (header and any trailing array included) and is a multiple of 8, so
 * records stay 8-byte aligned. Records with a trailing array declare it as the
 * last member with length 0.
 *
 * Replay semantics:
 *  - Draw state (pipeline, viewports, scissors, topology, vertex/index buffers,
 *    root arguments, blend factor, stencil reference) persists across records
 *    and is re-applied automatically whenever a new render encoder is opened.
 *  - Records are D3D12-shaped. The backend owns render passes: SET_RENDER_TARGETS
 *    and CLEAR_RTV only update its state, a render encoder opens at the first
 *    draw (pending clears become load actions) and closes when the targets
 *    change, a copy arrives or the submit ends. Clears that never meet a draw
 *    run as clear-only passes. Setting the same targets again keeps the pass.
 *    Copy records open and close a blit encoder lazily.
 *  - GPU addresses are mtlb_buffer_info::gpu_address values (buffer base plus
 *    offset); the backend resolves them back to the owning buffer.
 */
#ifndef MTLB_CMD_H
#define MTLB_CMD_H

#include "mtlb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mtlb_cmd_type {
    MTLB_CMD_SET_RENDER_TARGETS = 1,
    MTLB_CMD_CLEAR_RTV,
    MTLB_CMD_SET_PIPELINE,
    MTLB_CMD_SET_VIEWPORTS,
    MTLB_CMD_SET_SCISSORS,
    MTLB_CMD_SET_TOPOLOGY,
    MTLB_CMD_SET_VERTEX_BUFFERS,
    MTLB_CMD_SET_INDEX_BUFFER,
    MTLB_CMD_SET_GRAPHICS_ROOT_ARGS,
    MTLB_CMD_SET_BLEND_FACTOR,
    MTLB_CMD_SET_STENCIL_REF,
    MTLB_CMD_DRAW,
    MTLB_CMD_DRAW_INDEXED,
    MTLB_CMD_COPY_BUFFER,
    MTLB_CMD_COPY_TEXTURE_TO_BUFFER,
    MTLB_CMD_COPY_BUFFER_TO_TEXTURE,
    MTLB_CMD_RESET_STATE,
    MTLB_CMD_SET_DESCRIPTOR_HEAPS,
    MTLB_CMD_COPY_TEXTURE_TEXTURE,
    MTLB_CMD_SET_COMPUTE_ROOT_ARGS,
    MTLB_CMD_DISPATCH,
    MTLB_CMD_CLEAR_BUFFER,
    MTLB_CMD_CLEAR_TEXTURE_UAV,
    MTLB_CMD_CLEAR_DSV,
    MTLB_CMD_BARRIER,
    MTLB_CMD_EXECUTE_INDIRECT,
    MTLB_CMD_RESOLVE,
    MTLB_CMD_BEGIN_QUERY,
    MTLB_CMD_END_QUERY,
    MTLB_CMD_RESOLVE_QUERY,
    MTLB_CMD_MARKER,
    MTLB_CMD_WRITE_IMMEDIATE,
    MTLB_CMD_GDEFLATE,
} mtlb_cmd_type;

typedef struct mtlb_cmd_header {
    uint32_t type;   /* mtlb_cmd_type */
    uint32_t size;
} mtlb_cmd_header;

static inline uint32_t mtlb_cmd_align(uint32_t size)
{
    return (size + 7u) & ~7u;
}

/* Resets the persistent draw state to its defaults. The front-end starts every
 * command list with one, so state never leaks from one list to the next within
 * a single submit. Also unbinds the render targets. */
typedef struct mtlb_cmd_reset_state {
    mtlb_cmd_header header;
} mtlb_cmd_reset_state;

/* ---- Render targets ------------------------------------------------------ */

/* A render target view: one mip and slice of a texture, viewed as `view_format`. */
typedef struct mtlb_render_target {
    mtlb_texture texture;      /* 0 leaves the slot unbound */
    uint32_t view_format;      /* mtlb_format to view the texture as; 0 = texture's own */
    uint32_t mip_level;
    uint32_t array_slice;
    uint32_t reserved;
} mtlb_render_target;

enum {
    MTLB_DEPTH_READ_ONLY = 1u << 0,    /* the depth plane is only read */
    MTLB_STENCIL_READ_ONLY = 1u << 1,
};

/* Binds the colour targets and the depth-stencil view (texture 0: none) for later draws. */
typedef struct mtlb_cmd_set_render_targets {
    mtlb_cmd_header header;
    uint32_t count;            /* at most MTLB_MAX_RENDER_TARGETS */
    uint32_t depth_flags;      /* MTLB_DEPTH_READ_ONLY | MTLB_STENCIL_READ_ONLY */
    mtlb_render_target depth;
    mtlb_render_target targets[];
} mtlb_cmd_set_render_targets;

enum {
    MTLB_CLEAR_DEPTH = 1u << 0,
    MTLB_CLEAR_STENCIL = 1u << 1,
};

/* Clears the depth and/or stencil plane of a view. The view need not be bound. */
typedef struct mtlb_cmd_clear_dsv {
    mtlb_cmd_header header;
    mtlb_render_target target;
    uint32_t flags;            /* MTLB_CLEAR_* */
    float depth;
    uint32_t stencil;
    uint32_t reserved;
} mtlb_cmd_clear_dsv;

/* Clears one view. The view need not be bound. */
typedef struct mtlb_cmd_clear_rtv {
    mtlb_cmd_header header;
    mtlb_render_target target;
    float color[4];
} mtlb_cmd_clear_rtv;

/* ---- Draw state --------------------------------------------------------- */

typedef struct mtlb_cmd_set_pipeline {
    mtlb_cmd_header header;
    mtlb_pipeline pipeline;
} mtlb_cmd_set_pipeline;

typedef struct mtlb_viewport {
    float x, y, width, height, min_depth, max_depth;
} mtlb_viewport;

typedef struct mtlb_cmd_set_viewports {
    mtlb_cmd_header header;
    uint32_t count;
    uint32_t reserved;
    mtlb_viewport viewports[];
} mtlb_cmd_set_viewports;

typedef struct mtlb_rect {
    int32_t left, top, right, bottom;
} mtlb_rect;

typedef struct mtlb_cmd_set_scissors {
    mtlb_cmd_header header;
    uint32_t count;
    uint32_t reserved;
    mtlb_rect rects[];
} mtlb_cmd_set_scissors;

/* Values equal D3D_PRIMITIVE_TOPOLOGY for the supported topologies. */
typedef enum mtlb_topology {
    MTLB_TOPOLOGY_POINT_LIST = 1,
    MTLB_TOPOLOGY_LINE_LIST = 2,
    MTLB_TOPOLOGY_LINE_STRIP = 3,
    MTLB_TOPOLOGY_TRIANGLE_LIST = 4,
    MTLB_TOPOLOGY_TRIANGLE_STRIP = 5,
    MTLB_TOPOLOGY_LINE_LIST_ADJ = 10,
    MTLB_TOPOLOGY_LINE_STRIP_ADJ = 11,
    MTLB_TOPOLOGY_TRIANGLE_LIST_ADJ = 12,
    MTLB_TOPOLOGY_TRIANGLE_STRIP_ADJ = 13,
    MTLB_TOPOLOGY_PATCH_LIST_1 = 33,  /* 1 to 32 control points: 33 to 64 */
    MTLB_TOPOLOGY_PATCH_LIST_32 = 64,
} mtlb_topology;

typedef struct mtlb_cmd_set_topology {
    mtlb_cmd_header header;
    uint32_t topology;         /* mtlb_topology */
    uint32_t reserved;
} mtlb_cmd_set_topology;

typedef struct mtlb_vertex_buffer {
    uint64_t gpu_address;      /* 0 unbinds the slot */
    uint32_t size;
    uint32_t stride;
} mtlb_vertex_buffer;

typedef struct mtlb_cmd_set_vertex_buffers {
    mtlb_cmd_header header;
    uint32_t start_slot;
    uint32_t count;
    mtlb_vertex_buffer buffers[];
} mtlb_cmd_set_vertex_buffers;

typedef struct mtlb_cmd_set_index_buffer {
    mtlb_cmd_header header;
    uint64_t gpu_address;      /* 0 unbinds */
    uint32_t size;
    uint32_t index_size;       /* 2 or 4 bytes */
} mtlb_cmd_set_index_buffer;

/* Snapshot of the whole graphics top-level argument buffer, laid out as the
 * Metal shader converter expects for the pipeline's root signature (root
 * constants inline, root descriptors and descriptor tables as 64-bit GPU
 * addresses). Bound to the vertex and fragment stages at the argument buffer
 * bind point. */
typedef struct mtlb_cmd_set_graphics_root_args {
    mtlb_cmd_header header;
    uint32_t data_size;
    uint32_t reserved;
    uint8_t data[];
} mtlb_cmd_set_graphics_root_args;

typedef struct mtlb_cmd_set_blend_factor {
    mtlb_cmd_header header;
    float factor[4];
} mtlb_cmd_set_blend_factor;

typedef struct mtlb_cmd_set_stencil_ref {
    mtlb_cmd_header header;
    uint32_t ref;
    uint32_t reserved;
} mtlb_cmd_set_stencil_ref;

typedef struct mtlb_cmd_draw {
    mtlb_cmd_header header;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t start_vertex;
    uint32_t start_instance;
} mtlb_cmd_draw;

typedef struct mtlb_cmd_draw_indexed {
    mtlb_cmd_header header;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t start_index;
    int32_t base_vertex;
    uint32_t start_instance;
    uint32_t reserved;
} mtlb_cmd_draw_indexed;

/* ---- Copies ------------------------------------------------------------- */

typedef struct mtlb_cmd_copy_buffer {
    mtlb_cmd_header header;
    mtlb_buffer dst;
    mtlb_buffer src;
    uint64_t dst_offset;
    uint64_t src_offset;
    uint64_t size;
} mtlb_cmd_copy_buffer;

/* Texel region of one subresource, plus the placed-footprint buffer layout. */
typedef struct mtlb_texture_copy_region {
    mtlb_texture texture;
    mtlb_buffer buffer;
    uint64_t buffer_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    uint32_t mip_level;
    uint32_t array_slice;
    uint32_t x, y, z;
    uint32_t width, height, depth;
    uint32_t plane;            /* depth-stencil textures: 0 depth, 1 stencil; else 0 */
} mtlb_texture_copy_region;

/* Used by both MTLB_CMD_COPY_TEXTURE_TO_BUFFER and MTLB_CMD_COPY_BUFFER_TO_TEXTURE;
 * the record type gives the direction. */
typedef struct mtlb_cmd_copy_texture {
    mtlb_cmd_header header;
    mtlb_texture_copy_region region;
} mtlb_cmd_copy_texture;

/* The descriptor heaps shader code may index directly (SM 6.6 ResourceDescriptorHeap and
 * SamplerDescriptorHeap) and that root descriptor tables point into. Addresses are those of
 * the heaps' first descriptors; 0 = none. */
typedef struct mtlb_cmd_set_descriptor_heaps {
    mtlb_cmd_header header;
    uint64_t resource_heap;
    uint64_t sampler_heap;
} mtlb_cmd_set_descriptor_heaps;

/* Texture to texture copy: a box of one subresource into another, or (whole = 1) every mip
 * and slice of the source into the destination, which must have the same layout. */
typedef struct mtlb_cmd_copy_texture_texture {
    mtlb_cmd_header header;
    mtlb_texture dst;
    mtlb_texture src;
    uint32_t dst_mip, dst_slice, src_mip, src_slice;
    uint32_t dst_x, dst_y, dst_z;
    uint32_t src_x, src_y, src_z;
    uint32_t width, height, depth;
    uint32_t whole;
} mtlb_cmd_copy_texture_texture;

/* Compute work. SET_PIPELINE binds a compute pipeline when its handle is one. The compute root
 * arguments are a snapshot like the graphics ones, with the same record layout
 * (mtlb_cmd_set_graphics_root_args). */
typedef struct mtlb_cmd_dispatch {
    mtlb_cmd_header header;
    uint32_t x, y, z;
    uint32_t reserved;
} mtlb_cmd_dispatch;

/* UAV clears. A buffer range is filled with the repeated bytes of one element; a texture view (a
 * mip level, a range of slices) is cleared to the value, written as float, uint or int per `kind`. */
typedef struct mtlb_cmd_clear_buffer {
    mtlb_cmd_header header;
    mtlb_buffer buffer;
    uint64_t offset;           /* bytes */
    uint64_t size;             /* bytes, a multiple of pattern_size */
    uint32_t pattern_size;     /* 1..16 */
    uint32_t reserved;
    uint8_t pattern[16];
} mtlb_cmd_clear_buffer;

enum { MTLB_CLEAR_FLOAT = 0, MTLB_CLEAR_UINT = 1, MTLB_CLEAR_SINT = 2 };

typedef struct mtlb_cmd_clear_texture_uav {
    mtlb_cmd_header header;
    mtlb_texture texture;
    mtlb_texture_view_desc view;
    uint32_t kind;             /* MTLB_CLEAR_* */
    uint32_t reserved;
    uint32_t value[4];         /* the bits of 4 floats, uints or ints */
    uint32_t x, y, width, height;  /* region of the level; width 0 = all of it */
} mtlb_cmd_clear_texture_uav;

/* Resource barriers. Resources are not hazard tracked: work on the queue is ordered by explicit
 * synchronisation, which a barrier asks for. Between two encoders nothing waits unless a barrier (or a
 * command list boundary came in between); a barrier ends the open blit encoder and orders the dispatches of the
 * open compute encoder around it. It ends the open render pass too, unless the pass already waited for everything
 * before it, none of its draws binds a UAV and the barrier names nothing the pass renders to (draws that follow then
 * continue in the same pass). */
enum {
    MTLB_BARRIER_TRANSITION = 1,
    MTLB_BARRIER_UAV = 2,
    MTLB_BARRIER_ALIASING = 3,
};

typedef struct mtlb_barrier {
    uint32_t type;             /* MTLB_BARRIER_* */
    uint32_t reserved;
    mtlb_texture texture;      /* the resource the barrier names, a texture or a buffer; both 0: all resources */
    mtlb_buffer buffer;
} mtlb_barrier;

typedef struct mtlb_cmd_barrier {
    mtlb_cmd_header header;
    uint32_t count;
    uint32_t reserved;
    mtlb_barrier barriers[];
} mtlb_cmd_barrier;

/* ExecuteIndirect. The commands are read by the GPU from an argument buffer, `stride` bytes apart: each starts
 * with the arguments that change root arguments or vertex buffers (described by `args`), then the action's
 * arguments at `action_src_offset` in the D3D12 layouts (draw: 4 dwords, indexed draw: 5, dispatch: 3).
 * `max_count` commands run; if `count_address` is not 0 the number actually run is the dword stored there
 * (at most max_count). */
enum {
    MTLB_INDIRECT_DRAW = 1,
    MTLB_INDIRECT_DRAW_INDEXED = 2,
    MTLB_INDIRECT_DISPATCH = 3,
};

enum {
    MTLB_INDIRECT_ARG_CONSTANT = 1,    /* `size` bytes copied to dst_offset of the root arguments */
    MTLB_INDIRECT_ARG_POINTER = 2,     /* a GPU address (8 bytes) copied to dst_offset of the root arguments */
    MTLB_INDIRECT_ARG_VERTEX_BUFFER = 3, /* a D3D12_VERTEX_BUFFER_VIEW for slot dst_offset */
    MTLB_INDIRECT_ARG_COMMAND_INDEX = 4, /* the command's index, to dst_offset of the root arguments */
};

typedef struct mtlb_indirect_arg {
    uint32_t type;             /* MTLB_INDIRECT_ARG_* */
    uint32_t dst_offset;
    uint32_t size;
    uint32_t src_offset;       /* in the command */
} mtlb_indirect_arg;

typedef struct mtlb_cmd_execute_indirect {
    mtlb_cmd_header header;
    uint32_t action;           /* MTLB_INDIRECT_* */
    uint32_t max_count;
    uint32_t stride;
    uint32_t action_src_offset;
    uint64_t arg_address;      /* GPU address of the first command */
    uint64_t count_address;    /* GPU address of the count, or 0 */
    uint32_t num_args;
    uint32_t reserved;
    mtlb_indirect_arg args[];
} mtlb_cmd_execute_indirect;

/* Resolves one subresource of a multisampled texture into a single-sampled one of the same size, as
 * `format` (a mtlb_format; 0 = the textures' own). */
typedef struct mtlb_cmd_resolve {
    mtlb_cmd_header header;
    mtlb_texture dst;
    mtlb_texture src;
    uint32_t dst_mip, dst_slice, src_mip, src_slice;
    uint32_t format;
    uint32_t reserved;
} mtlb_cmd_resolve;

/* Queries. Occlusion counts are written by the render passes between BEGIN and END; a timestamp is taken when
 * the work before the END record has finished; statistics queries have no data (zeros). RESOLVE copies the
 * results of `count` queries from `start` to a buffer as D3D12 lays them out (8 bytes each; pipeline statistics
 * 88, stream output statistics 16). */
typedef struct mtlb_cmd_query {
    mtlb_cmd_header header;
    mtlb_query_heap heap;
    uint32_t type;             /* MTLB_QUERY_* */
    uint32_t index;
} mtlb_cmd_query;

typedef struct mtlb_cmd_resolve_query {
    mtlb_cmd_header header;
    mtlb_query_heap heap;
    mtlb_buffer dst;
    uint64_t dst_offset;
    uint32_t type;
    uint32_t start;
    uint32_t count;
    uint32_t reserved;
} mtlb_cmd_resolve_query;

/* A debug group boundary or label: kind 0 push, 1 pop, 2 signpost; `text` is NUL-terminated within the record. */
typedef struct mtlb_cmd_marker {
    mtlb_cmd_header header;
    uint32_t kind;
    uint32_t length;
    char text[];
} mtlb_cmd_marker;

/* WriteBufferImmediate: a 32- or 64-bit value stored at a GPU address, after the work recorded before it. */
typedef struct mtlb_cmd_write_immediate {
    mtlb_cmd_header header;
    uint64_t address;
    uint64_t value;
    uint32_t size;             /* 4 or 8 */
    uint32_t reserved;
} mtlb_cmd_write_immediate;

/* The DirectStorage GDeflate meta command (ExecuteMetaCommand): decompresses `stream_count` GDeflate streams.
 * The control buffer starts with a dword stream count (the smaller of that and `stream_count` is decompressed),
 * then {input offset, output offset} dwords per stream, offsets relative to the input and output addresses. Each
 * stream is a GDeflate tile stream (8-byte header, a dword offset per tile, 64 KiB tiles). The scratch buffer
 * must hold mtlb_gdeflate_scratch_size() bytes for the streams of the call. Runs as two compute kernels that
 * are ordered with the work before and after it; the buffers are named by GPU address and checked against the
 * buffers they fall into. */
typedef struct mtlb_cmd_gdeflate {
    mtlb_cmd_header header;
    uint64_t input_address, input_size;
    uint64_t output_address, output_size;
    uint64_t control_address, control_size;
    uint64_t scratch_address, scratch_size;
    uint64_t stream_count;
} mtlb_cmd_gdeflate;

/* Scratch bytes for `streams` streams: a header of 4 dwords (the dispatch of the decode kernel and the stream
 * count), then the number of tiles before each stream and in all. */
static inline uint64_t mtlb_gdeflate_scratch_size(uint32_t streams)
{
    return 16 + 4 * ((uint64_t)streams + 1);
}

MTLB_ASSERT_SIZE(mtlb_cmd_gdeflate, 80);
MTLB_ASSERT_SIZE(mtlb_cmd_header, 8);
MTLB_ASSERT_SIZE(mtlb_cmd_resolve, 48);
MTLB_ASSERT_SIZE(mtlb_cmd_query, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_resolve_query, 48);
MTLB_ASSERT_SIZE(mtlb_cmd_marker, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_write_immediate, 32);
MTLB_ASSERT_SIZE(mtlb_indirect_arg, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_execute_indirect, 48);
MTLB_ASSERT_OFFSET(mtlb_cmd_execute_indirect, args, 48);
MTLB_ASSERT_SIZE(mtlb_barrier, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_barrier, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_clear_buffer, 56);
MTLB_ASSERT_SIZE(mtlb_cmd_clear_texture_uav, 88);
MTLB_ASSERT_SIZE(mtlb_cmd_dispatch, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_descriptor_heaps, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_copy_texture_texture, 80);
MTLB_ASSERT_SIZE(mtlb_render_target, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_render_targets, 40);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_render_targets, targets, 40);
MTLB_ASSERT_SIZE(mtlb_cmd_clear_dsv, 48);
MTLB_ASSERT_SIZE(mtlb_cmd_clear_rtv, 48);
MTLB_ASSERT_OFFSET(mtlb_cmd_clear_rtv, color, 32);
MTLB_ASSERT_SIZE(mtlb_cmd_reset_state, 8);
MTLB_ASSERT_SIZE(mtlb_cmd_set_pipeline, 16);
MTLB_ASSERT_SIZE(mtlb_viewport, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_viewports, 16);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_viewports, viewports, 16);
MTLB_ASSERT_SIZE(mtlb_rect, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_set_scissors, 16);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_scissors, rects, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_set_topology, 16);
MTLB_ASSERT_SIZE(mtlb_vertex_buffer, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_set_vertex_buffers, 16);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_vertex_buffers, buffers, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_set_index_buffer, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_graphics_root_args, 16);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_graphics_root_args, data, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_set_blend_factor, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_stencil_ref, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_draw, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_draw_indexed, 32);
MTLB_ASSERT_SIZE(mtlb_cmd_copy_buffer, 48);
MTLB_ASSERT_SIZE(mtlb_texture_copy_region, 72);
MTLB_ASSERT_OFFSET(mtlb_texture_copy_region, buffer_offset, 16);
MTLB_ASSERT_SIZE(mtlb_cmd_copy_texture, 80);

#ifdef __cplusplus
}
#endif

#endif /* MTLB_CMD_H */
