// Queues and command stream replay.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#define IR_PRIVATE_IMPLEMENTATION
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include "bridge/mtlb_cmd.h"

namespace {

using namespace mtlb;

MTLPrimitiveType to_primitive_type(uint32_t topology)
{
    switch (topology) {
    case MTLB_TOPOLOGY_POINT_LIST: return MTLPrimitiveTypePoint;
    case MTLB_TOPOLOGY_LINE_LIST: return MTLPrimitiveTypeLine;
    case MTLB_TOPOLOGY_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case MTLB_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

// Returns the texture, or a cached view of it when `view_format` asks for another format.
id<MTLTexture> attachment_texture(Texture *texture, uint32_t view_format)
{
    if (view_format == 0 || view_format == texture->format)
        return texture->texture;
    std::lock_guard<std::mutex> lock(texture->views_mutex);
    auto &view = texture->views[view_format];
    if (!view) {
        MTLPixelFormat format = to_pixel_format(view_format);
        if (format != MTLPixelFormatInvalid)
            view = [texture->texture newTextureViewWithPixelFormat:format];
    }
    return view;
}

// True when `count` trailing elements of type E fit in the record `cmd`.
template <class E, class T>
bool array_fits(const T &cmd, uint64_t count)
{
    return count <= (cmd.header.size - sizeof(T)) / sizeof(E);
}

// A render target view with its texture resolved.
struct Target {
    Texture *texture = nullptr;
    uint32_t view_format = 0;  // 0 = the texture's own format
    uint32_t mip_level = 0;
    uint32_t array_slice = 0;

    bool operator==(const Target &o) const
    {
        return texture == o.texture && view_format == o.view_format && mip_level == o.mip_level
               && array_slice == o.array_slice;
    }
};

// The state that persists from record to record. A fresh DrawState is what a
// command list starts from.
struct DrawState {
    Target targets[MTLB_MAX_RENDER_TARGETS];
    uint32_t num_targets = 0;
    Pipeline *pipeline = nullptr;
    MTLViewport viewports[MTLB_MAX_VIEWPORTS];
    uint32_t num_viewports = 0;
    mtlb_rect scissors[MTLB_MAX_VIEWPORTS];
    uint32_t num_scissors = 0;
    uint32_t topology = MTLB_TOPOLOGY_TRIANGLE_LIST;
    IRRuntimeVertexBuffers vertex_buffers = {};  // read by the stage-in function
    id<MTLBuffer> index_buffer = nil;            // nil: unbound or not a known address
    uint64_t index_offset = 0;
    uint32_t index_size = 0;
    // Points into the submitted stream, which outlives the replay.
    const uint8_t *root_args = nullptr;
    uint32_t root_args_size = 0;
    float blend_factor[4] = {1, 1, 1, 1};
    uint32_t stencil_ref = 0;
};

// Replays one command stream into a command buffer, opening and closing
// encoders on demand and re-applying draw state on every new render encoder.
class Replay {
public:
    Replay(Queue *queue, id<MTLCommandBuffer> command_buffer) : queue_(queue), cb_(command_buffer) {}

    mtlb_result run(const uint8_t *stream, size_t length);
    void finish()
    {
        end_blit();
        end_render();
        flush_clears(false);
    }

private:
    enum Dirty : uint32_t {
        kPipeline = 1u << 0,
        kViewports = 1u << 1,
        kScissors = 1u << 2,
        kRootArgs = 1u << 3,
        kBlendFactor = 1u << 4,
        kStencilRef = 1u << 5,
        kVertexBuffers = 1u << 6,
        kAll = (kVertexBuffers << 1) - 1,
    };

    // A CLEAR_RTV waiting for the next pass that binds its view (or a clear-only pass).
    struct PendingClear {
        Target target;
        float color[4];
    };

    void mark_all_dirty() { dirty_ = kAll; }

    mtlb_result execute(const mtlb_cmd_header *header);
    // Checks that a record is large enough for its type, then calls the handler.
    template <class T>
    mtlb_result dispatch(const mtlb_cmd_header *header, mtlb_result (Replay::*handler)(const T &));

    mtlb_result reset_state(const mtlb_cmd_reset_state &cmd);
    mtlb_result set_render_targets(const mtlb_cmd_set_render_targets &cmd);
    mtlb_result clear_rtv(const mtlb_cmd_clear_rtv &cmd);
    mtlb_result set_pipeline(const mtlb_cmd_set_pipeline &cmd);
    mtlb_result set_viewports(const mtlb_cmd_set_viewports &cmd);
    mtlb_result set_scissors(const mtlb_cmd_set_scissors &cmd);
    mtlb_result set_topology(const mtlb_cmd_set_topology &cmd);
    mtlb_result set_vertex_buffers(const mtlb_cmd_set_vertex_buffers &cmd);
    mtlb_result set_index_buffer(const mtlb_cmd_set_index_buffer &cmd);
    mtlb_result set_root_args(const mtlb_cmd_set_graphics_root_args &cmd);
    mtlb_result set_blend_factor(const mtlb_cmd_set_blend_factor &cmd);
    mtlb_result set_stencil_ref(const mtlb_cmd_set_stencil_ref &cmd);
    mtlb_result draw(const mtlb_cmd_draw &cmd);
    mtlb_result draw_indexed(const mtlb_cmd_draw_indexed &cmd);
    mtlb_result copy_buffer(const mtlb_cmd_copy_buffer &cmd);
    mtlb_result copy_texture_to_buffer(const mtlb_cmd_copy_texture &cmd);
    mtlb_result copy_buffer_to_texture(const mtlb_cmd_copy_texture &cmd);

    mtlb_result resolve_target(const mtlb_render_target &t, Target *out);
    bool is_bound(const Target &t) const;
    mtlb_result open_render_pass();
    mtlb_result flush_clears(bool keep_bound);
    mtlb_result clear_only_pass(const PendingClear &clear);
    mtlb_result begin_draw(bool *ready);
    mtlb_result apply_state();
    mtlb_result copy_texture(const mtlb_texture_copy_region &r, bool to_buffer);
    id<MTLBlitCommandEncoder> blit();
    id<MTLRenderCommandEncoder> new_render_encoder(MTLRenderPassDescriptor *pass);

    void end_blit()
    {
        [blit_ endEncoding];
        blit_ = nil;
    }

    void end_render()
    {
        [render_ endEncoding];
        render_ = nil;
    }

    Queue *queue_;
    id<MTLCommandBuffer> cb_;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;

    DrawState state_;
    std::vector<PendingClear> clears_;  // waiting for a pass that binds their view
    bool warned_no_targets_ = false;
    uint32_t dirty_ = kAll;  // state_ pieces the current encoder has not seen
    NSUInteger target_width_ = 0, target_height_ = 0;
};

mtlb_result Replay::run(const uint8_t *stream, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < sizeof(mtlb_cmd_header))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "truncated command header");
        auto *cmd = reinterpret_cast<const mtlb_cmd_header *>(stream + offset);
        if (cmd->size < sizeof(mtlb_cmd_header) || cmd->size > length - offset || (cmd->size & 7))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "bad command size " + std::to_string(cmd->size));
        mtlb_result result = execute(cmd);
        if (result != MTLB_OK)
            return result;
        offset += cmd->size;
    }
    return MTLB_OK;
}

// The blit encoder, opened on demand. Copies end the render pass and run any
// pending clears first so they observe the cleared contents.
id<MTLBlitCommandEncoder> Replay::blit()
{
    if (!blit_) {
        end_render();
        flush_clears(false);
        blit_ = [cb_ blitCommandEncoder];
    }
    return blit_;
}

id<MTLRenderCommandEncoder> Replay::new_render_encoder(MTLRenderPassDescriptor *pass)
{
    end_blit();
    queue_->render_passes.fetch_add(1, std::memory_order_relaxed);
    return [cb_ renderCommandEncoderWithDescriptor:pass];
}

mtlb_result Replay::resolve_target(const mtlb_render_target &t, Target *out)
{
    *out = {};
    if (!t.texture)
        return MTLB_OK;  // unbound slot
    Texture *texture = from_handle<Texture>(t.texture);
    if (!texture)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid render target texture");
    *out = {texture, t.view_format == texture->format ? 0u : t.view_format, t.mip_level, t.array_slice};
    return MTLB_OK;
}

bool Replay::is_bound(const Target &t) const
{
    return std::find(state_.targets, state_.targets + state_.num_targets, t) != state_.targets + state_.num_targets;
}

mtlb_result Replay::set_render_targets(const mtlb_cmd_set_render_targets &cmd)
{
    if (cmd.count > MTLB_MAX_RENDER_TARGETS || !array_fits<mtlb_render_target>(cmd, cmd.count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "render target count out of range");
    Target targets[MTLB_MAX_RENDER_TARGETS];
    for (uint32_t i = 0; i < cmd.count; ++i) {
        mtlb_result result = resolve_target(cmd.targets[i], &targets[i]);
        if (result != MTLB_OK)
            return result;
    }
    // Binding the same targets again must not break the pass.
    if (cmd.count == state_.num_targets && std::equal(targets, targets + cmd.count, state_.targets))
        return MTLB_OK;

    end_render();
    std::copy(targets, targets + cmd.count, state_.targets);
    std::fill(state_.targets + cmd.count, state_.targets + MTLB_MAX_RENDER_TARGETS, Target{});
    state_.num_targets = cmd.count;
    // Clears for views that are no longer bound can only run as passes of their own.
    return flush_clears(true);
}

mtlb_result Replay::clear_rtv(const mtlb_cmd_clear_rtv &cmd)
{
    Target target;
    mtlb_result result = resolve_target(cmd.target, &target);
    if (result != MTLB_OK)
        return result;
    if (!target.texture)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "CLEAR_RTV without a texture");
    // Draws may already have landed in the open pass; the clear must come after them.
    if (render_ && is_bound(target))
        end_render();
    // A newer clear of the same view supersedes a pending one.
    clears_.erase(std::remove_if(clears_.begin(), clears_.end(),
                                 [&](const PendingClear &c) { return c.target == target; }),
                  clears_.end());
    PendingClear clear{target, {}};
    std::copy_n(cmd.color, 4, clear.color);
    clears_.push_back(clear);
    return MTLB_OK;
}

// Runs pending clears as passes of their own. With `keep_bound`, clears of the
// current targets stay pending so the next pass can fold them into load actions.
mtlb_result Replay::flush_clears(bool keep_bound)
{
    end_render();
    for (auto it = clears_.begin(); it != clears_.end();) {
        if (keep_bound && is_bound(it->target)) {
            ++it;
            continue;
        }
        mtlb_result result = clear_only_pass(*it);
        it = clears_.erase(it);
        if (result != MTLB_OK)
            return result;
    }
    return MTLB_OK;
}

mtlb_result Replay::clear_only_pass(const PendingClear &clear)
{
    id<MTLTexture> view = attachment_texture(clear.target.texture, clear.target.view_format);
    if (!view)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported attachment view format");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[0];
    ca.texture = view;
    ca.level = clear.target.mip_level;
    ca.slice = clear.target.array_slice;
    ca.loadAction = MTLLoadActionClear;
    ca.storeAction = MTLStoreActionStore;
    ca.clearColor = MTLClearColorMake(clear.color[0], clear.color[1], clear.color[2], clear.color[3]);
    [new_render_encoder(pass) endEncoding];
    return MTLB_OK;
}

// Opens the render encoder for the current targets, turning pending clears of
// bound views into load actions.
mtlb_result Replay::open_render_pass()
{
    // Clears of views this pass does not bind cannot join it.
    mtlb_result result = flush_clears(true);
    if (result != MTLB_OK)
        return result;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    target_width_ = target_height_ = 0;
    for (uint32_t i = 0; i < state_.num_targets; ++i) {
        const Target &t = state_.targets[i];
        if (!t.texture)
            continue;
        id<MTLTexture> view = attachment_texture(t.texture, t.view_format);
        if (!view)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported attachment view format");
        MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[i];
        ca.texture = view;
        ca.level = t.mip_level;
        ca.slice = t.array_slice;
        ca.loadAction = MTLLoadActionLoad;
        ca.storeAction = MTLStoreActionStore;
        auto clear = std::find_if(clears_.begin(), clears_.end(), [&](const PendingClear &c) { return c.target == t; });
        if (clear != clears_.end()) {
            ca.loadAction = MTLLoadActionClear;
            ca.clearColor = MTLClearColorMake(clear->color[0], clear->color[1], clear->color[2], clear->color[3]);
            clears_.erase(clear);
        }
        NSUInteger w = std::max<NSUInteger>(view.width >> t.mip_level, 1);
        NSUInteger h = std::max<NSUInteger>(view.height >> t.mip_level, 1);
        target_width_ = target_width_ ? std::min(target_width_, w) : w;
        target_height_ = target_height_ ? std::min(target_height_, h) : h;
    }

    render_ = new_render_encoder(pass);
    if (!render_)
        return fail(MTLB_ERROR_DEVICE, "renderCommandEncoderWithDescriptor failed");
    mark_all_dirty();
    return MTLB_OK;
}

// Pushes the state changed since the last draw (or everything, on a new encoder).
mtlb_result Replay::apply_state()
{
    if (!render_) {
        mtlb_result result = open_render_pass();
        if (result != MTLB_OK)
            return result;
    }

    if (dirty_ & kPipeline) {
        [render_ setRenderPipelineState:state_.pipeline->state];
        [render_ setDepthStencilState:state_.pipeline->depth_stencil];
        [render_ setCullMode:state_.pipeline->cull_mode];
        [render_ setFrontFacingWinding:state_.pipeline->winding];
        [render_ setTriangleFillMode:state_.pipeline->fill_mode];
        [render_ setDepthClipMode:state_.pipeline->depth_clip];
        [render_ setDepthBias:state_.pipeline->depth_bias slopeScale:state_.pipeline->slope_scaled_depth_bias clamp:state_.pipeline->depth_bias_clamp];
    }
    if ((dirty_ & kViewports) && state_.num_viewports)
        [render_ setViewports:state_.viewports count:state_.num_viewports];
    if ((dirty_ & kScissors) && state_.num_scissors) {
        // Metal requires scissors inside the render target; D3D12 does not.
        MTLScissorRect rects[MTLB_MAX_VIEWPORTS];
        for (uint32_t i = 0; i < state_.num_scissors; ++i) {
            const mtlb_rect &r = state_.scissors[i];
            NSUInteger left = std::clamp<int32_t>(r.left, 0, static_cast<int32_t>(target_width_));
            NSUInteger top = std::clamp<int32_t>(r.top, 0, static_cast<int32_t>(target_height_));
            NSUInteger right = std::clamp<int32_t>(r.right, static_cast<int32_t>(left), static_cast<int32_t>(target_width_));
            NSUInteger bottom = std::clamp<int32_t>(r.bottom, static_cast<int32_t>(top), static_cast<int32_t>(target_height_));
            rects[i] = {left, top, right - left, bottom - top};
        }
        [render_ setScissorRects:rects count:state_.num_scissors];
    }
    if ((dirty_ & kRootArgs) && state_.root_args_size) {
        [render_ setVertexBytes:state_.root_args length:state_.root_args_size atIndex:kIRArgumentBufferBindPoint];
        [render_ setFragmentBytes:state_.root_args length:state_.root_args_size atIndex:kIRArgumentBufferBindPoint];
    }
    if (dirty_ & kBlendFactor)
        [render_ setBlendColorRed:state_.blend_factor[0] green:state_.blend_factor[1] blue:state_.blend_factor[2] alpha:state_.blend_factor[3]];
    if (dirty_ & kStencilRef)
        [render_ setStencilReferenceValue:state_.stencil_ref];
    if (dirty_ & kVertexBuffers)
        [render_ setVertexBytes:state_.vertex_buffers length:sizeof(state_.vertex_buffers) atIndex:kIRVertexBufferBindPoint];
    dirty_ = 0;
    return MTLB_OK;
}

// Gets a draw ready: validates it, opens the pass and applies state. Sets
// *ready to false for draws that are skipped: those without render targets
// (reported once), since depth-only rendering is not supported yet.
mtlb_result Replay::begin_draw(bool *ready)
{
    *ready = false;
    if (!state_.pipeline)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "draw without a pipeline");
    if (std::none_of(state_.targets, state_.targets + state_.num_targets, [](const Target &t) { return t.texture; })) {
        if (!warned_no_targets_)
            std::fprintf(stderr, "d3d12-metal: draw skipped, no render targets are bound\n");
        warned_no_targets_ = true;
        return MTLB_OK;
    }
    *ready = true;
    return apply_state();
}

mtlb_result Replay::draw(const mtlb_cmd_draw &cmd)
{
    bool ready;
    mtlb_result result = begin_draw(&ready);
    if (result != MTLB_OK || !ready)
        return result;
    IRRuntimeDrawPrimitives(render_, to_primitive_type(state_.topology), cmd.start_vertex, cmd.vertex_count,
                            cmd.instance_count, cmd.start_instance);
    return MTLB_OK;
}

mtlb_result Replay::draw_indexed(const mtlb_cmd_draw_indexed &cmd)
{
    bool ready;
    mtlb_result result = begin_draw(&ready);
    if (result != MTLB_OK || !ready)
        return result;
    if (!state_.index_buffer || (state_.index_size != 2 && state_.index_size != 4))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indexed draw without a valid index buffer");
    IRRuntimeDrawIndexedPrimitives(render_, to_primitive_type(state_.topology), cmd.index_count,
                                   state_.index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, state_.index_buffer,
                                   state_.index_offset + uint64_t(cmd.start_index) * state_.index_size, cmd.instance_count,
                                   cmd.base_vertex, cmd.start_instance);
    return MTLB_OK;
}

mtlb_result Replay::copy_texture(const mtlb_texture_copy_region &r, bool to_buffer)
{
    Texture *texture = from_handle<Texture>(r.texture);
    Buffer *buffer = from_handle<Buffer>(r.buffer);
    id<MTLBlitCommandEncoder> enc = blit();
    if (!texture || !buffer || !enc)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid texture copy");
    MTLOrigin origin = {r.x, r.y, r.z};
    MTLSize size = {r.width, r.height, r.depth};
    if (to_buffer) {
        [enc copyFromTexture:texture->texture sourceSlice:r.array_slice sourceLevel:r.mip_level
                sourceOrigin:origin sourceSize:size toBuffer:buffer->buffer destinationOffset:r.buffer_offset
           destinationBytesPerRow:r.bytes_per_row destinationBytesPerImage:r.bytes_per_image];
    } else {
        [enc copyFromBuffer:buffer->buffer sourceOffset:r.buffer_offset sourceBytesPerRow:r.bytes_per_row
            sourceBytesPerImage:r.bytes_per_image sourceSize:size toTexture:texture->texture
           destinationSlice:r.array_slice destinationLevel:r.mip_level destinationOrigin:origin];
    }
    return MTLB_OK;
}

template <class T>
mtlb_result Replay::dispatch(const mtlb_cmd_header *header, mtlb_result (Replay::*handler)(const T &))
{
    if (header->size < sizeof(T))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "command " + std::to_string(header->type) + " is too small");
    return (this->*handler)(*reinterpret_cast<const T *>(header));
}

mtlb_result Replay::reset_state(const mtlb_cmd_reset_state &)
{
    // Whatever the previous list left (open pass, pending clears) completes first.
    mtlb_result result = flush_clears(false);
    if (result != MTLB_OK)
        return result;
    state_ = DrawState{};
    mark_all_dirty();
    return MTLB_OK;
}

mtlb_result Replay::set_pipeline(const mtlb_cmd_set_pipeline &cmd)
{
    auto *pipeline = from_handle<Pipeline>(cmd.pipeline);
    if (!pipeline)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid pipeline handle");
    if (pipeline != state_.pipeline)
        dirty_ |= kPipeline;
    state_.pipeline = pipeline;
    return MTLB_OK;
}

mtlb_result Replay::set_viewports(const mtlb_cmd_set_viewports &cmd)
{
    if (cmd.count > MTLB_MAX_VIEWPORTS || !array_fits<mtlb_viewport>(cmd, cmd.count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "viewport count out of range");
    state_.num_viewports = cmd.count;
    for (uint32_t i = 0; i < cmd.count; ++i) {
        const mtlb_viewport &v = cmd.viewports[i];
        state_.viewports[i] = {v.x, v.y, v.width, v.height, v.min_depth, v.max_depth};
    }
    dirty_ |= kViewports;
    return MTLB_OK;
}

mtlb_result Replay::set_scissors(const mtlb_cmd_set_scissors &cmd)
{
    if (cmd.count > MTLB_MAX_VIEWPORTS || !array_fits<mtlb_rect>(cmd, cmd.count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "scissor count out of range");
    state_.num_scissors = cmd.count;
    std::copy_n(cmd.rects, cmd.count, state_.scissors);
    dirty_ |= kScissors;
    return MTLB_OK;
}

mtlb_result Replay::set_topology(const mtlb_cmd_set_topology &cmd)
{
    state_.topology = cmd.topology;
    return MTLB_OK;
}

mtlb_result Replay::set_vertex_buffers(const mtlb_cmd_set_vertex_buffers &cmd)
{
    if (uint64_t(cmd.start_slot) + cmd.count > MTLB_MAX_VERTEX_BUFFERS || !array_fits<mtlb_vertex_buffer>(cmd, cmd.count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "vertex buffer slots out of range");
    for (uint32_t i = 0; i < cmd.count; ++i)
        state_.vertex_buffers[cmd.start_slot + i] = {cmd.buffers[i].gpu_address, cmd.buffers[i].size, cmd.buffers[i].stride};
    dirty_ |= kVertexBuffers;
    return MTLB_OK;
}

mtlb_result Replay::set_index_buffer(const mtlb_cmd_set_index_buffer &cmd)
{
    // Resolve the address once here rather than on every indexed draw.
    Buffer *buffer = cmd.gpu_address ? find_buffer(queue_->device, cmd.gpu_address, &state_.index_offset) : nullptr;
    state_.index_buffer = buffer ? buffer->buffer : nil;
    state_.index_size = cmd.index_size;
    return MTLB_OK;
}

mtlb_result Replay::set_root_args(const mtlb_cmd_set_graphics_root_args &cmd)
{
    if (!array_fits<uint8_t>(cmd, cmd.data_size))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "root argument size exceeds the record");
    state_.root_args = cmd.data;
    state_.root_args_size = cmd.data_size;
    dirty_ |= kRootArgs;
    return MTLB_OK;
}

mtlb_result Replay::set_blend_factor(const mtlb_cmd_set_blend_factor &cmd)
{
    std::copy_n(cmd.factor, 4, state_.blend_factor);
    dirty_ |= kBlendFactor;
    return MTLB_OK;
}

mtlb_result Replay::set_stencil_ref(const mtlb_cmd_set_stencil_ref &cmd)
{
    state_.stencil_ref = cmd.ref;
    dirty_ |= kStencilRef;
    return MTLB_OK;
}

mtlb_result Replay::copy_buffer(const mtlb_cmd_copy_buffer &cmd)
{
    Buffer *dst = from_handle<Buffer>(cmd.dst), *src = from_handle<Buffer>(cmd.src);
    if (!dst || !src)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid buffer copy");
    [blit() copyFromBuffer:src->buffer sourceOffset:cmd.src_offset toBuffer:dst->buffer
         destinationOffset:cmd.dst_offset size:cmd.size];
    return MTLB_OK;
}

mtlb_result Replay::copy_texture_to_buffer(const mtlb_cmd_copy_texture &cmd)
{
    return copy_texture(cmd.region, true);
}

mtlb_result Replay::copy_buffer_to_texture(const mtlb_cmd_copy_texture &cmd)
{
    return copy_texture(cmd.region, false);
}

mtlb_result Replay::execute(const mtlb_cmd_header *header)
{
    switch (header->type) {
    case MTLB_CMD_RESET_STATE: return dispatch(header, &Replay::reset_state);
    case MTLB_CMD_SET_RENDER_TARGETS: return dispatch(header, &Replay::set_render_targets);
    case MTLB_CMD_CLEAR_RTV: return dispatch(header, &Replay::clear_rtv);
    case MTLB_CMD_SET_PIPELINE: return dispatch(header, &Replay::set_pipeline);
    case MTLB_CMD_SET_VIEWPORTS: return dispatch(header, &Replay::set_viewports);
    case MTLB_CMD_SET_SCISSORS: return dispatch(header, &Replay::set_scissors);
    case MTLB_CMD_SET_TOPOLOGY: return dispatch(header, &Replay::set_topology);
    case MTLB_CMD_SET_VERTEX_BUFFERS: return dispatch(header, &Replay::set_vertex_buffers);
    case MTLB_CMD_SET_INDEX_BUFFER: return dispatch(header, &Replay::set_index_buffer);
    case MTLB_CMD_SET_GRAPHICS_ROOT_ARGS: return dispatch(header, &Replay::set_root_args);
    case MTLB_CMD_SET_BLEND_FACTOR: return dispatch(header, &Replay::set_blend_factor);
    case MTLB_CMD_SET_STENCIL_REF: return dispatch(header, &Replay::set_stencil_ref);
    case MTLB_CMD_DRAW: return dispatch(header, &Replay::draw);
    case MTLB_CMD_DRAW_INDEXED: return dispatch(header, &Replay::draw_indexed);
    case MTLB_CMD_COPY_BUFFER: return dispatch(header, &Replay::copy_buffer);
    case MTLB_CMD_COPY_TEXTURE_TO_BUFFER: return dispatch(header, &Replay::copy_texture_to_buffer);
    case MTLB_CMD_COPY_BUFFER_TO_TEXTURE: return dispatch(header, &Replay::copy_buffer_to_texture);
    default: return fail(MTLB_ERROR_INVALID_ARGUMENT, "unknown command type " + std::to_string(header->type));
    }
}

// Bounds how much work a lazily committed command buffer may accumulate.
constexpr uint32_t kMaxOpenSubmits = 32;

// The queue's open command buffer, created on first use. Work stays in it until
// something needs the GPU to see it (a signal, enough submits, queue teardown).
// The buffer retains what it references: an application may release a pipeline
// or fence as soon as it sees a fence signalled, which can be before the buffer
// retires, and Metal's validation layer rejects that for unretained buffers.
id<MTLCommandBuffer> open_command_buffer(Queue *queue)
{
    if (!queue->open) {
        commit_residency(queue->device);
        queue->open = [queue->queue commandBuffer];
        [queue->open addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.error)
                std::fprintf(stderr, "d3d12-metal: command buffer failed: %s\n", done.error.localizedDescription.UTF8String);
        }];
    }
    return queue->open;
}

void commit_open(Queue *queue)
{
    if (!queue->open)
        return;
    commit_residency(queue->device);
    [queue->open commit];
    queue->open = nil;
    queue->open_submits = 0;
}

} // namespace

extern "C" {

mtlb_result mtlb_queue_create(mtlb_device handle, mtlb_queue *out)
{
    Device *device = from_handle<Device>(handle);
    if (!device || !out)
        return MTLB_ERROR_INVALID_ARGUMENT;
    id<MTLCommandQueue> mtl_queue = [device->device newCommandQueueWithMaxCommandBufferCount:1024];
    if (!mtl_queue)
        return fail(MTLB_ERROR_DEVICE, "newCommandQueue failed");
    [mtl_queue addResidencySet:device->residency];
    auto *queue = new Queue();
    queue->device = device;
    queue->queue = mtl_queue;
    *out = to_handle(queue);
    return MTLB_OK;
}

void mtlb_queue_destroy(mtlb_queue handle)
{
    Queue *queue = from_handle<Queue>(handle);
    if (!queue)
        return;
    commit_open(queue);
    [queue->queue removeResidencySet:queue->device->residency];
    delete queue;
}

mtlb_result mtlb_queue_submit(mtlb_queue handle, const mtlb_span *spans, uint32_t count)
{
    Queue *queue = from_handle<Queue>(handle);
    if (!queue || (!spans && count))
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(queue->mutex);
    Replay replay(queue, open_command_buffer(queue));
    mtlb_result result = MTLB_OK;
    for (uint32_t i = 0; i < count && result == MTLB_OK; ++i) {
        if (!spans[i].data && spans[i].size)
            result = fail(MTLB_ERROR_INVALID_ARGUMENT, "span without data");
        else
            result = replay.run(spans[i].data, spans[i].size);
    }
    replay.finish();  // work encoded before a failure stays in the open buffer
    if (++queue->open_submits >= kMaxOpenSubmits)
        commit_open(queue);
    return result;
}

uint64_t mtlb_queue_render_pass_count(mtlb_queue handle)
{
    Queue *queue = from_handle<Queue>(handle);
    return queue ? queue->render_passes.load() : 0;
}

// Appends a signal to the open buffer and commits it, so a signal after
// ExecuteCommandLists costs one commit for both.
mtlb_result mtlb_queue_signal(mtlb_queue handle, mtlb_event event_handle, uint64_t value)
{
    Queue *queue = from_handle<Queue>(handle);
    Event *event = from_handle<Event>(event_handle);
    if (!queue || !event)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(queue->mutex);
    [open_command_buffer(queue) encodeSignalEvent:event->event value:value];
    commit_open(queue);
    return MTLB_OK;
}

// Appends a wait to the open buffer. It is committed with the next signal.
mtlb_result mtlb_queue_wait(mtlb_queue handle, mtlb_event event_handle, uint64_t value)
{
    Queue *queue = from_handle<Queue>(handle);
    Event *event = from_handle<Event>(event_handle);
    if (!queue || !event)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(queue->mutex);
    [open_command_buffer(queue) encodeWaitForEvent:event->event value:value];
    return MTLB_OK;
}

} // extern "C"
