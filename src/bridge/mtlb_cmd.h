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

/* Binds the colour targets for later draws. Depth-stencil views are not
 * recorded yet (DSV milestone). */
typedef struct mtlb_cmd_set_render_targets {
    mtlb_cmd_header header;
    uint32_t count;            /* at most MTLB_MAX_RENDER_TARGETS */
    uint32_t reserved;
    mtlb_render_target targets[];
} mtlb_cmd_set_render_targets;

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
    uint32_t reserved;
} mtlb_texture_copy_region;

typedef struct mtlb_cmd_copy_texture_to_buffer {
    mtlb_cmd_header header;
    mtlb_texture_copy_region region;
} mtlb_cmd_copy_texture_to_buffer;

typedef struct mtlb_cmd_copy_buffer_to_texture {
    mtlb_cmd_header header;
    mtlb_texture_copy_region region;
} mtlb_cmd_copy_buffer_to_texture;

MTLB_ASSERT_SIZE(mtlb_cmd_header, 8);
MTLB_ASSERT_SIZE(mtlb_render_target, 24);
MTLB_ASSERT_SIZE(mtlb_cmd_set_render_targets, 16);
MTLB_ASSERT_OFFSET(mtlb_cmd_set_render_targets, targets, 16);
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
MTLB_ASSERT_SIZE(mtlb_cmd_copy_texture_to_buffer, 80);
MTLB_ASSERT_SIZE(mtlb_cmd_copy_buffer_to_texture, 80);

#ifdef __cplusplus
}
#endif

#endif /* MTLB_CMD_H */
