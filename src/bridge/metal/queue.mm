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

MTLLoadAction to_load_action(uint32_t action)
{
    switch (action) {
    case MTLB_LOAD_CLEAR: return MTLLoadActionClear;
    case MTLB_LOAD_DONT_CARE: return MTLLoadActionDontCare;
    default: return MTLLoadActionLoad;
    }
}

MTLStoreAction to_store_action(uint32_t action)
{
    return action == MTLB_STORE_DONT_CARE ? MTLStoreActionDontCare : MTLStoreActionStore;
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
    void finish() { end_blit(); end_render(); }

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

    mtlb_result execute(const mtlb_cmd_header *cmd);
    mtlb_result reset_state();
    mtlb_result begin_render_pass(const mtlb_cmd_begin_render_pass *cmd);
    mtlb_result apply_state();
    mtlb_result draw(const mtlb_cmd_draw *cmd);
    mtlb_result draw_indexed(const mtlb_cmd_draw_indexed *cmd);
    mtlb_result copy_texture(const mtlb_texture_copy_region &r, bool to_buffer);
    id<MTLBlitCommandEncoder> blit();

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

id<MTLBlitCommandEncoder> Replay::blit()
{
    if (render_)
        return nil;
    if (!blit_)
        blit_ = [cb_ blitCommandEncoder];
    return blit_;
}

mtlb_result Replay::begin_render_pass(const mtlb_cmd_begin_render_pass *cmd)
{
    if (render_)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "nested render pass");
    if (cmd->num_colors > MTLB_MAX_RENDER_TARGETS)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "too many colour attachments");
    end_blit();

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    target_width_ = target_height_ = 0;
    auto note_size = [&](id<MTLTexture> t, uint32_t mip) {
        NSUInteger w = std::max<NSUInteger>(t.width >> mip, 1), h = std::max<NSUInteger>(t.height >> mip, 1);
        target_width_ = target_width_ ? std::min(target_width_, w) : w;
        target_height_ = target_height_ ? std::min(target_height_, h) : h;
    };

    for (uint32_t i = 0; i < cmd->num_colors; ++i) {
        const mtlb_color_attachment &a = cmd->colors[i];
        Texture *texture = from_handle<Texture>(a.texture);
        if (!texture)
            continue;  // unbound slot
        id<MTLTexture> view = attachment_texture(texture, a.view_format);
        if (!view)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported attachment view format");
        MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[i];
        ca.texture = view;
        ca.level = a.mip_level;
        ca.slice = a.array_slice;
        ca.loadAction = to_load_action(a.load_action);
        ca.storeAction = to_store_action(a.store_action);
        ca.clearColor = MTLClearColorMake(a.clear_color[0], a.clear_color[1], a.clear_color[2], a.clear_color[3]);
        note_size(view, a.mip_level);
    }

    if (cmd->has_depth) {
        const mtlb_depth_attachment &d = cmd->depth;
        Texture *texture = from_handle<Texture>(d.texture);
        if (!texture)
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid depth attachment");
        id<MTLTexture> view = attachment_texture(texture, d.view_format);
        if (!view)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported attachment view format");
        pass.depthAttachment.texture = view;
        pass.depthAttachment.level = d.mip_level;
        pass.depthAttachment.slice = d.array_slice;
        pass.depthAttachment.loadAction = to_load_action(d.depth_load_action);
        pass.depthAttachment.storeAction = to_store_action(d.depth_store_action);
        pass.depthAttachment.clearDepth = d.clear_depth;
        if (view.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            pass.stencilAttachment.texture = view;
            pass.stencilAttachment.level = d.mip_level;
            pass.stencilAttachment.slice = d.array_slice;
            pass.stencilAttachment.loadAction = to_load_action(d.stencil_load_action);
            pass.stencilAttachment.storeAction = to_store_action(d.stencil_store_action);
            pass.stencilAttachment.clearStencil = d.clear_stencil;
        }
        note_size(view, d.mip_level);
    }

    render_ = [cb_ renderCommandEncoderWithDescriptor:pass];
    if (!render_)
        return fail(MTLB_ERROR_DEVICE, "renderCommandEncoderWithDescriptor failed");
    dirty_ = kAll;
    return MTLB_OK;
}

// Pushes the state changed since the last draw (or everything, on a new encoder).
mtlb_result Replay::apply_state()
{
    if (!render_)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "draw outside a render pass");

    if (dirty_ & kPipeline) {
        if (!pipeline_)
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "draw without a pipeline");
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

mtlb_result Replay::draw(const mtlb_cmd_draw *cmd)
{
    mtlb_result result = apply_state();
    if (result != MTLB_OK)
        return result;
    IRRuntimeDrawPrimitives(render_, to_primitive_type(topology_), cmd->start_vertex, cmd->vertex_count,
                            cmd->instance_count, cmd->start_instance);
    return MTLB_OK;
}

mtlb_result Replay::draw_indexed(const mtlb_cmd_draw_indexed *cmd)
{
    mtlb_result result = apply_state();
    if (result != MTLB_OK)
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
    case MTLB_CMD_BEGIN_RENDER_PASS: return sizeof(mtlb_cmd_begin_render_pass);
    case MTLB_CMD_END_RENDER_PASS: return sizeof(mtlb_cmd_end_render_pass);
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
    if (render_)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "RESET_STATE inside a render pass");
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
    case MTLB_CMD_BEGIN_RENDER_PASS:
        return begin_render_pass(reinterpret_cast<const mtlb_cmd_begin_render_pass *>(header));
    case MTLB_CMD_END_RENDER_PASS:
        if (!render_)
            return fail(MTLB_ERROR_INVALID_ARGUMENT, "END_RENDER_PASS without a render pass");
        end_render();
        return MTLB_OK;
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
