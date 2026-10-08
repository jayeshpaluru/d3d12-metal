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
        kAll = 0x7fu,
    };

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

    // A CLEAR_RTV waiting for the next pass that binds its view (or a clear-only pass).
    struct PendingClear {
        Target target;
        float color[4];
    };

    mtlb_result execute(const mtlb_cmd_header *cmd);
    mtlb_result reset_state();
    mtlb_result resolve_target(const mtlb_render_target &t, Target *out);
    mtlb_result set_render_targets(const mtlb_cmd_set_render_targets *cmd);
    mtlb_result clear_rtv(const mtlb_cmd_clear_rtv *cmd);
    bool is_bound(const Target &t) const;
    mtlb_result open_render_pass();
    mtlb_result flush_clears(bool keep_bound);
    mtlb_result clear_only_pass(const PendingClear &clear);
    mtlb_result begin_draw(bool *ready);
    mtlb_result apply_state();
    mtlb_result draw(const mtlb_cmd_draw *cmd);
    mtlb_result draw_indexed(const mtlb_cmd_draw_indexed *cmd);
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

    // Render targets and the clears waiting to run.
    Target targets_[MTLB_MAX_RENDER_TARGETS];
    uint32_t num_targets_ = 0;
    std::vector<PendingClear> clears_;
    bool warned_no_targets_ = false;

    // Persistent draw state.
    Pipeline *pipeline_ = nullptr;
    std::vector<MTLViewport> viewports_;
    std::vector<mtlb_rect> scissors_;
    uint32_t topology_ = MTLB_TOPOLOGY_TRIANGLE_LIST;
    IRRuntimeVertexBuffers vertex_buffers_ = {};  // read by the stage-in function
    uint64_t index_address_ = 0;
    uint32_t index_size_ = 0;
    std::vector<uint8_t> root_args_;
    float blend_factor_[4] = {1, 1, 1, 1};
    uint32_t stencil_ref_ = 0;
    uint32_t dirty_ = kAll;
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
    return std::find(targets_, targets_ + num_targets_, t) != targets_ + num_targets_;
}

mtlb_result Replay::set_render_targets(const mtlb_cmd_set_render_targets *cmd)
{
    Target targets[MTLB_MAX_RENDER_TARGETS];
    for (uint32_t i = 0; i < cmd->count; ++i) {
        mtlb_result result = resolve_target(cmd->targets[i], &targets[i]);
        if (result != MTLB_OK)
            return result;
    }
    // Binding the same targets again must not break the pass.
    if (cmd->count == num_targets_ && std::equal(targets, targets + cmd->count, targets_))
        return MTLB_OK;

    end_render();
    std::copy(targets, targets + cmd->count, targets_);
    std::fill(targets_ + cmd->count, targets_ + MTLB_MAX_RENDER_TARGETS, Target{});
    num_targets_ = cmd->count;
    // Clears for views that are no longer bound can only run as passes of their own.
    return flush_clears(true);
}

mtlb_result Replay::clear_rtv(const mtlb_cmd_clear_rtv *cmd)
{
    Target target;
    mtlb_result result = resolve_target(cmd->target, &target);
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
    std::copy_n(cmd->color, 4, clear.color);
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
    for (uint32_t i = 0; i < num_targets_; ++i) {
        const Target &t = targets_[i];
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
    dirty_ = kAll;
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
        [render_ setRenderPipelineState:pipeline_->state];
        [render_ setDepthStencilState:pipeline_->depth_stencil];
        [render_ setCullMode:pipeline_->cull_mode];
        [render_ setFrontFacingWinding:pipeline_->winding];
        [render_ setTriangleFillMode:pipeline_->fill_mode];
        [render_ setDepthClipMode:pipeline_->depth_clip];
        [render_ setDepthBias:pipeline_->depth_bias slopeScale:pipeline_->slope_scaled_depth_bias clamp:pipeline_->depth_bias_clamp];
    }
    if ((dirty_ & kViewports) && !viewports_.empty())
        [render_ setViewports:viewports_.data() count:viewports_.size()];
    if ((dirty_ & kScissors) && !scissors_.empty()) {
        // Metal requires scissors inside the render target; D3D12 does not.
        std::vector<MTLScissorRect> rects;
        for (const mtlb_rect &r : scissors_) {
            NSUInteger left = std::clamp<int32_t>(r.left, 0, static_cast<int32_t>(target_width_));
            NSUInteger top = std::clamp<int32_t>(r.top, 0, static_cast<int32_t>(target_height_));
            NSUInteger right = std::clamp<int32_t>(r.right, static_cast<int32_t>(left), static_cast<int32_t>(target_width_));
            NSUInteger bottom = std::clamp<int32_t>(r.bottom, static_cast<int32_t>(top), static_cast<int32_t>(target_height_));
            rects.push_back({left, top, right - left, bottom - top});
        }
        [render_ setScissorRects:rects.data() count:rects.size()];
    }
    if ((dirty_ & kRootArgs) && !root_args_.empty()) {
        [render_ setVertexBytes:root_args_.data() length:root_args_.size() atIndex:kIRArgumentBufferBindPoint];
        [render_ setFragmentBytes:root_args_.data() length:root_args_.size() atIndex:kIRArgumentBufferBindPoint];
    }
    if (dirty_ & kBlendFactor)
        [render_ setBlendColorRed:blend_factor_[0] green:blend_factor_[1] blue:blend_factor_[2] alpha:blend_factor_[3]];
    if (dirty_ & kStencilRef)
        [render_ setStencilReferenceValue:stencil_ref_];
    if (dirty_ & kVertexBuffers)
        [render_ setVertexBytes:vertex_buffers_ length:sizeof(vertex_buffers_) atIndex:kIRVertexBufferBindPoint];
    dirty_ = 0;
    return MTLB_OK;
}

// Gets a draw ready: validates it, opens the pass and applies state. Sets
// *ready to false for draws that are skipped: those without render targets
// (reported once), since depth-only rendering is not supported yet.
mtlb_result Replay::begin_draw(bool *ready)
{
    *ready = false;
    if (!pipeline_)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "draw without a pipeline");
    if (std::none_of(targets_, targets_ + num_targets_, [](const Target &t) { return t.texture; })) {
        if (!warned_no_targets_)
            std::fprintf(stderr, "d3d12-metal: draw skipped, no render targets are bound\n");
        warned_no_targets_ = true;
        return MTLB_OK;
    }
    *ready = true;
    return apply_state();
}

mtlb_result Replay::draw(const mtlb_cmd_draw *cmd)
{
    bool ready;
    mtlb_result result = begin_draw(&ready);
    if (result != MTLB_OK || !ready)
        return result;
    IRRuntimeDrawPrimitives(render_, to_primitive_type(topology_), cmd->start_vertex, cmd->vertex_count,
                            cmd->instance_count, cmd->start_instance);
    return MTLB_OK;
}

mtlb_result Replay::draw_indexed(const mtlb_cmd_draw_indexed *cmd)
{
    bool ready;
    mtlb_result result = begin_draw(&ready);
    if (result != MTLB_OK || !ready)
        return result;
    uint64_t offset = 0;
    Buffer *buffer = index_address_ ? find_buffer(queue_->device, index_address_, &offset) : nullptr;
    if (!buffer || (index_size_ != 2 && index_size_ != 4))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indexed draw without a valid index buffer");
    IRRuntimeDrawIndexedPrimitives(render_, to_primitive_type(topology_), cmd->index_count,
                                   index_size_ == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, buffer->buffer,
                                   offset + uint64_t(cmd->start_index) * index_size_, cmd->instance_count,
                                   cmd->base_vertex, cmd->start_instance);
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

// Size of the fixed part of each record type, or 0 for an unknown type.
size_t fixed_size(uint32_t type)
{
    switch (type) {
    case MTLB_CMD_SET_RENDER_TARGETS: return sizeof(mtlb_cmd_set_render_targets);
    case MTLB_CMD_CLEAR_RTV: return sizeof(mtlb_cmd_clear_rtv);
    case MTLB_CMD_SET_PIPELINE: return sizeof(mtlb_cmd_set_pipeline);
    case MTLB_CMD_SET_VIEWPORTS: return sizeof(mtlb_cmd_set_viewports);
    case MTLB_CMD_SET_SCISSORS: return sizeof(mtlb_cmd_set_scissors);
    case MTLB_CMD_SET_TOPOLOGY: return sizeof(mtlb_cmd_set_topology);
    case MTLB_CMD_SET_VERTEX_BUFFERS: return sizeof(mtlb_cmd_set_vertex_buffers);
    case MTLB_CMD_SET_INDEX_BUFFER: return sizeof(mtlb_cmd_set_index_buffer);
    case MTLB_CMD_SET_GRAPHICS_ROOT_ARGS: return sizeof(mtlb_cmd_set_graphics_root_args);
    case MTLB_CMD_SET_BLEND_FACTOR: return sizeof(mtlb_cmd_set_blend_factor);
    case MTLB_CMD_SET_STENCIL_REF: return sizeof(mtlb_cmd_set_stencil_ref);
    case MTLB_CMD_DRAW: return sizeof(mtlb_cmd_draw);
    case MTLB_CMD_DRAW_INDEXED: return sizeof(mtlb_cmd_draw_indexed);
    case MTLB_CMD_COPY_BUFFER: return sizeof(mtlb_cmd_copy_buffer);
    case MTLB_CMD_COPY_TEXTURE_TO_BUFFER: return sizeof(mtlb_cmd_copy_texture_to_buffer);
    case MTLB_CMD_COPY_BUFFER_TO_TEXTURE: return sizeof(mtlb_cmd_copy_buffer_to_texture);
    case MTLB_CMD_RESET_STATE: return sizeof(mtlb_cmd_reset_state);
    default: return 0;
    }
}

// True when `count` elements of `element_size` bytes fit after the fixed part
// of a record of `size` bytes.
bool array_fits(uint32_t size, size_t fixed, uint64_t count, size_t element_size)
{
    return count <= (size - fixed) / element_size;
}

mtlb_result Replay::reset_state()
{
    // Whatever the previous list left (open pass, pending clears) completes first.
    mtlb_result result = flush_clears(false);
    if (result != MTLB_OK)
        return result;
    num_targets_ = 0;
    std::fill(targets_, targets_ + MTLB_MAX_RENDER_TARGETS, Target{});
    pipeline_ = nullptr;
    viewports_.clear();
    scissors_.clear();
    topology_ = MTLB_TOPOLOGY_TRIANGLE_LIST;
    std::fill(std::begin(vertex_buffers_), std::end(vertex_buffers_), IRRuntimeVertexBuffer{});
    index_address_ = 0;
    index_size_ = 0;
    root_args_.clear();
    std::fill_n(blend_factor_, 4, 1.0f);
    stencil_ref_ = 0;
    dirty_ = kAll;
    return MTLB_OK;
}

mtlb_result Replay::execute(const mtlb_cmd_header *header)
{
    const size_t fixed = fixed_size(header->type);
    if (!fixed)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "unknown command type " + std::to_string(header->type));
    if (header->size < fixed)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "command " + std::to_string(header->type) + " is too small");

    switch (header->type) {
    case MTLB_CMD_RESET_STATE:
        return reset_state();
    case MTLB_CMD_SET_RENDER_TARGETS: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_render_targets *>(header);
        if (cmd->count > MTLB_MAX_RENDER_TARGETS || !array_fits(header->size, fixed, cmd->count, sizeof(mtlb_render_target)))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "render target count out of range");
        return set_render_targets(cmd);
    }
    case MTLB_CMD_CLEAR_RTV:
        return clear_rtv(reinterpret_cast<const mtlb_cmd_clear_rtv *>(header));
    case MTLB_CMD_SET_PIPELINE: {
        pipeline_ = from_handle<Pipeline>(reinterpret_cast<const mtlb_cmd_set_pipeline *>(header)->pipeline);
        dirty_ |= kPipeline;
        return pipeline_ ? MTLB_OK : fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid pipeline handle");
    }
    case MTLB_CMD_SET_VIEWPORTS: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_viewports *>(header);
        if (!array_fits(header->size, fixed, cmd->count, sizeof(mtlb_viewport)))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "viewport count exceeds the record");
        viewports_.clear();
        for (uint32_t i = 0; i < cmd->count; ++i) {
            const mtlb_viewport &v = cmd->viewports[i];
            viewports_.push_back({v.x, v.y, v.width, v.height, v.min_depth, v.max_depth});
        }
        dirty_ |= kViewports;
        return MTLB_OK;
    }
    case MTLB_CMD_SET_SCISSORS: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_scissors *>(header);
        if (!array_fits(header->size, fixed, cmd->count, sizeof(mtlb_rect)))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "scissor count exceeds the record");
        scissors_.assign(cmd->rects, cmd->rects + cmd->count);
        dirty_ |= kScissors;
        return MTLB_OK;
    }
    case MTLB_CMD_SET_TOPOLOGY:
        topology_ = reinterpret_cast<const mtlb_cmd_set_topology *>(header)->topology;
        return MTLB_OK;
    case MTLB_CMD_SET_VERTEX_BUFFERS: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_vertex_buffers *>(header);
        if (uint64_t(cmd->start_slot) + cmd->count > MTLB_MAX_VERTEX_BUFFERS
            || !array_fits(header->size, fixed, cmd->count, sizeof(mtlb_vertex_buffer)))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "vertex buffer slots out of range");
        for (uint32_t i = 0; i < cmd->count; ++i) {
            vertex_buffers_[cmd->start_slot + i] = {cmd->buffers[i].gpu_address, cmd->buffers[i].size,
                                                    cmd->buffers[i].stride};
        }
        dirty_ |= kVertexBuffers;
        return MTLB_OK;
    }
    case MTLB_CMD_SET_INDEX_BUFFER: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_index_buffer *>(header);
        index_address_ = cmd->gpu_address;
        index_size_ = cmd->index_size;
        return MTLB_OK;
    }
    case MTLB_CMD_SET_GRAPHICS_ROOT_ARGS: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_set_graphics_root_args *>(header);
        if (!array_fits(header->size, fixed, cmd->data_size, 1))
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "root argument size exceeds the record");
        root_args_.assign(cmd->data, cmd->data + cmd->data_size);
        dirty_ |= kRootArgs;
        return MTLB_OK;
    }
    case MTLB_CMD_SET_BLEND_FACTOR:
        std::copy_n(reinterpret_cast<const mtlb_cmd_set_blend_factor *>(header)->factor, 4, blend_factor_);
        dirty_ |= kBlendFactor;
        return MTLB_OK;
    case MTLB_CMD_SET_STENCIL_REF:
        stencil_ref_ = reinterpret_cast<const mtlb_cmd_set_stencil_ref *>(header)->ref;
        dirty_ |= kStencilRef;
        return MTLB_OK;
    case MTLB_CMD_DRAW:
        return draw(reinterpret_cast<const mtlb_cmd_draw *>(header));
    case MTLB_CMD_DRAW_INDEXED:
        return draw_indexed(reinterpret_cast<const mtlb_cmd_draw_indexed *>(header));
    case MTLB_CMD_COPY_BUFFER: {
        auto *cmd = reinterpret_cast<const mtlb_cmd_copy_buffer *>(header);
        Buffer *dst = from_handle<Buffer>(cmd->dst), *src = from_handle<Buffer>(cmd->src);
        id<MTLBlitCommandEncoder> enc = blit();
        if (!dst || !src || !enc)
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid buffer copy");
        [enc copyFromBuffer:src->buffer sourceOffset:cmd->src_offset toBuffer:dst->buffer
          destinationOffset:cmd->dst_offset size:cmd->size];
        return MTLB_OK;
    }
    case MTLB_CMD_COPY_TEXTURE_TO_BUFFER:
        return copy_texture(reinterpret_cast<const mtlb_cmd_copy_texture_to_buffer *>(header)->region, true);
    case MTLB_CMD_COPY_BUFFER_TO_TEXTURE:
        return copy_texture(reinterpret_cast<const mtlb_cmd_copy_buffer_to_texture *>(header)->region, false);
    default:
        return MTLB_ERROR_INVALID_ARGUMENT;  // unreachable: fixed_size() rejects unknown types
    }
}

// Bounds how much work a lazily committed command buffer may accumulate.
constexpr uint32_t kMaxOpenSubmits = 32;

// The queue's open command buffer, created on first use. Work stays in it until
// something needs the GPU to see it (a signal, enough submits, queue teardown).
// D3D12 requires applications to keep resources alive while the GPU uses them,
// so the buffer does not retain what it references.
id<MTLCommandBuffer> open_command_buffer(Queue *queue)
{
    if (!queue->open) {
        commit_residency(queue->device);
        queue->open = [queue->queue commandBufferWithUnretainedReferences];
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
