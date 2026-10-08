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
 *  - Render encoders exist between BEGIN_RENDER_PASS and END_RENDER_PASS. Copy
 *    records open and close a blit encoder lazily and must not appear inside a
 *    render pass.
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
    MTLB_CMD_BEGIN_RENDER_PASS = 1,
    MTLB_CMD_END_RENDER_PASS,
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
    MTLB_CMD_SIGNAL_EVENT,
    MTLB_CMD_WAIT_EVENT,
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
 * a single submit. Must not appear inside a render pass. */
typedef struct mtlb_cmd_reset_state {
    mtlb_cmd_header header;
} mtlb_cmd_reset_state;

/* ---- Render passes ------------------------------------------------------ */

typedef enum mtlb_load_action {
    MTLB_LOAD_LOAD = 0,
    MTLB_LOAD_CLEAR = 1,
    MTLB_LOAD_DONT_CARE = 2,
} mtlb_load_action;

typedef enum mtlb_store_action {
    MTLB_STORE_STORE = 0,
    MTLB_STORE_DONT_CARE = 1,
} mtlb_store_action;

typedef struct mtlb_color_attachment {
    mtlb_texture texture;
    uint32_t view_format;      /* mtlb_format to view the texture as; 0 = texture's own */
    uint32_t mip_level;
    uint32_t array_slice;
    uint32_t load_action;      /* mtlb_load_action */
    uint32_t store_action;     /* mtlb_store_action */
    uint32_t reserved;
    float clear_color[4];
} mtlb_color_attachment;

typedef struct mtlb_depth_attachment {
    mtlb_texture texture;
    uint32_t view_format;
    uint32_t mip_level;
    uint32_t array_slice;
    uint32_t depth_load_action;
    uint32_t depth_store_action;
    uint32_t stencil_load_action;
    uint32_t stencil_store_action;
    float clear_depth;
    uint32_t clear_stencil;
} mtlb_depth_attachment;

typedef struct mtlb_cmd_begin_render_pass {
    mtlb_cmd_header header;
    uint32_t num_colors;
    uint32_t has_depth;
    mtlb_color_attachment colors[MTLB_MAX_RENDER_TARGETS];
    mtlb_depth_attachment depth;
} mtlb_cmd_begin_render_pass;

typedef struct mtlb_cmd_end_render_pass {
    mtlb_cmd_header header;
} mtlb_cmd_end_render_pass;

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

/* ---- Synchronisation ---------------------------------------------------- */

typedef struct mtlb_cmd_signal_event {
    mtlb_cmd_header header;
    mtlb_event event;
    uint64_t value;
} mtlb_cmd_signal_event;

typedef struct mtlb_cmd_wait_event {
    mtlb_cmd_header header;
    mtlb_event event;
    uint64_t value;
} mtlb_cmd_wait_event;

#ifdef __cplusplus
}
#endif

#endif /* MTLB_CMD_H */
