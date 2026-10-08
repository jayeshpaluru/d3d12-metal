// Queues and command stream replay.
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <optional>
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
        MTLPixelFormat format = to_view_pixel_format(texture->texture.pixelFormat, view_format);
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
    Target depth;                // the depth-stencil view; no texture: none
    uint32_t depth_flags = 0;    // MTLB_*_READ_ONLY
    Pipeline *pipeline = nullptr;
    MTLViewport viewports[MTLB_MAX_VIEWPORTS];
    uint32_t num_viewports = 0;
    mtlb_rect scissors[MTLB_MAX_VIEWPORTS];
    uint32_t num_scissors = 0;
    uint32_t topology = MTLB_TOPOLOGY_TRIANGLE_LIST;
    IRRuntimeVertexBuffers vertex_buffers = {};  // read by the stage-in function
    id<MTLBuffer> index_buffer = nil;            // nil: unbound or not a known address
    uint64_t index_offset = 0;
    uint32_t index_size = 0;       // bytes per index
    uint64_t index_view_size = 0;  // bytes the index buffer view covers
    // Points into the submitted stream, which outlives the replay.
    const uint8_t *root_args = nullptr;
    uint32_t root_args_size = 0;
    float blend_factor[4] = {1, 1, 1, 1};
    uint32_t stencil_ref = 0;
    Pipeline *compute_pipeline = nullptr;
    const uint8_t *compute_root_args = nullptr;
    uint32_t compute_root_args_size = 0;
    uint64_t resource_heap = 0, sampler_heap = 0;  // GPU addresses of the bound descriptor heaps
};

// Replays one command stream into a command buffer, opening and closing
// encoders on demand and re-applying draw state on every new render encoder.
id<MTLCommandBuffer> open_command_buffer(Queue *queue);
void commit_open(Queue *queue);

class Replay {
public:
    Replay(Queue *queue, id<MTLCommandBuffer> command_buffer) : queue_(queue), cb_(command_buffer) {}

    // Replays one stream. A record that is invalid in itself (a bad handle, an
    // unresolvable address) is skipped and the stream goes on; a structurally
    // malformed stream (bad size, truncation, unknown type) ends this stream only.
    // The first error is kept for error().
    void run(const uint8_t *stream, size_t length);
    void note_span_error() { note_error(fail(MTLB_ERROR_INVALID_ARGUMENT, "span without data")); }
    mtlb_result error() const { return error_; }
    const std::string &error_message() const { return error_message_; }
    void finish()
    {
        end_encoders();
        for (; debug_depth_ > 0; --debug_depth_)
            [cb_ popDebugGroup];
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
        kHeaps = 1u << 7,
        kQuery = 1u << 8,
        kAll = (kQuery << 1) - 1,
    };

    // A CLEAR_RTV or CLEAR_DSV waiting for the next pass that binds its view (or a clear-only pass).
    struct PendingClear {
        Target target;
        bool depth_stencil = false;
        float color[4] = {};      // colour clears
        uint32_t aspects = 0;     // depth-stencil clears: MTLB_CLEAR_DEPTH | MTLB_CLEAR_STENCIL
        float depth = 0;
        uint32_t stencil = 0;
    };

    void mark_all_dirty() { dirty_ = kAll; }

    mtlb_result execute(const mtlb_cmd_header *header);
    // Checks that a record is large enough for its type, then calls the handler.
    template <class T>
    mtlb_result dispatch(const mtlb_cmd_header *header, mtlb_result (Replay::*handler)(const T &));

    mtlb_result reset_state(const mtlb_cmd_reset_state &cmd);
    mtlb_result set_render_targets(const mtlb_cmd_set_render_targets &cmd);
    mtlb_result clear_rtv(const mtlb_cmd_clear_rtv &cmd);
    mtlb_result clear_dsv(const mtlb_cmd_clear_dsv &cmd);
    mtlb_result add_clear(const PendingClear &clear);
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
    mtlb_result set_compute_root_args(const mtlb_cmd_set_graphics_root_args &cmd);
    mtlb_result set_descriptor_heaps(const mtlb_cmd_set_descriptor_heaps &cmd);
    mtlb_result dispatch_compute(const mtlb_cmd_dispatch &cmd);
    mtlb_result barrier(const mtlb_cmd_barrier &cmd);
    mtlb_result resolve(const mtlb_cmd_resolve &cmd);
    mtlb_result begin_query(const mtlb_cmd_query &cmd);
    mtlb_result end_query(const mtlb_cmd_query &cmd);
    mtlb_result resolve_query(const mtlb_cmd_resolve_query &cmd);
    mtlb_result marker(const mtlb_cmd_marker &cmd);
    mtlb_result write_immediate(const mtlb_cmd_write_immediate &cmd);
    mtlb_result timestamp(QueryHeap *heap, uint32_t index);
    mtlb_result execute_indirect(const mtlb_cmd_execute_indirect &cmd);
    mtlb_result clear_buffer(const mtlb_cmd_clear_buffer &cmd);
    mtlb_result clear_texture_uav(const mtlb_cmd_clear_texture_uav &cmd);
    mtlb_result copy_texture_texture(const mtlb_cmd_copy_texture_texture &cmd);
    void bind_heaps(uint32_t stage_mask);
    mtlb_result copy_buffer(const mtlb_cmd_copy_buffer &cmd);
    mtlb_result copy_texture_to_buffer(const mtlb_cmd_copy_texture &cmd);
    mtlb_result copy_buffer_to_texture(const mtlb_cmd_copy_texture &cmd);

    mtlb_result resolve_target(const mtlb_render_target &t, Target *out);
    bool is_bound(const PendingClear &clear) const;
    void attach_depth(MTLRenderPassDescriptor *pass, const Target &t, id<MTLTexture> view, const PendingClear *clear);
    mtlb_result open_render_pass();
    mtlb_result flush_clears(bool keep_bound);
    mtlb_result clear_only_pass(const PendingClear &clear);
    mtlb_result begin_draw(bool *ready);
    mtlb_result apply_state();
    mtlb_result copy_texture(const mtlb_texture_copy_region &r, bool to_buffer);
    id<MTLBlitCommandEncoder> blit();
    id<MTLComputeCommandEncoder> compute();
    id<MTLComputeCommandEncoder> prepare_dispatch();
    id<MTLRenderCommandEncoder> new_render_encoder(MTLRenderPassDescriptor *pass);

    // Every encoder ends by updating the queue's fence and every encoder starts by waiting for the
    // one before (see Queue::fence).
    void end_blit()
    {
        if (!blit_)
            return;
        [blit_ updateFence:queue_->fence];
        queue_->fence_pending = true;
        [blit_ endEncoding];
        blit_ = nil;
    }

    void end_render()
    {
        if (!render_)
            return;
        [render_ updateFence:queue_->fence afterStages:MTLRenderStageFragment];
        queue_->fence_pending = true;
        [render_ endEncoding];
        render_ = nil;
    }

    void end_compute()
    {
        if (!compute_)
            return;
        [compute_ updateFence:queue_->fence];
        queue_->fence_pending = true;
        [compute_ endEncoding];
        compute_ = nil;
    }

    // Ends whatever encoder is open and runs the clears waiting for a pass.
    void end_encoders()
    {
        end_blit();
        end_compute();
        end_render();
        flush_clears(false);
    }

    void note_error(mtlb_result result);

    Queue *queue_;
    id<MTLCommandBuffer> cb_;
    mtlb_result error_ = MTLB_OK;
    std::string error_message_;
    bool abort_stream_ = false;  // set by structural errors
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    id<MTLComputeCommandEncoder> compute_ = nil;

    DrawState state_;
    std::vector<PendingClear> clears_;  // waiting for a pass that binds their view
    bool warned_no_targets_ = false;
    uint32_t dirty_ = kAll;  // state_ pieces the current render encoder has not seen
    uint32_t dirty_compute_ = kAll;  // the same for the compute encoder
    // The next encoder must wait for the one before it: a barrier or the start of a command list (command
    // lists may rely on D3D12's implicit state promotion and decay) came in between. A submission starts
    // with it set, as do the lists of one.
    // D3D12METAL_NO_BARRIERS=1 turns the synchronisation off (for showing that the barrier tests need it).
    const bool sync_disabled_ = getenv("D3D12METAL_NO_BARRIERS") != nullptr;
    bool sync_needed_ = !sync_disabled_;
    // The occlusion query in progress: render passes opened while it lasts count into its heap's buffer.
    QueryHeap *visibility_heap_ = nullptr;
    struct ActiveQuery {
        bool active = false;
        uint32_t index = 0;
        MTLVisibilityResultMode mode = MTLVisibilityResultModeDisabled;
    } query_;
    int debug_depth_ = 0;  // debug groups this submission has opened and not closed yet
    NSUInteger target_width_ = 0, target_height_ = 0;
    MTLPixelFormat pass_depth_format_ = MTLPixelFormatInvalid;    // attachments of the open pass
    MTLPixelFormat pass_stencil_format_ = MTLPixelFormatInvalid;
};

void Replay::note_error(mtlb_result result)
{
    if (error_ != MTLB_OK)
        return;
    error_ = result;
    error_message_ = mtlb_last_error();
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        std::fprintf(stderr, "d3d12-metal: invalid command skipped: %s\n", error_message_.c_str());
}

void Replay::run(const uint8_t *stream, size_t length)
{
    abort_stream_ = false;
    size_t offset = 0;
    while (offset < length && !abort_stream_) {
        if (length - offset < sizeof(mtlb_cmd_header)) {
            note_error(fail(MTLB_ERROR_INVALID_ARGUMENT, "truncated command header"));
            return;
        }
        auto *cmd = reinterpret_cast<const mtlb_cmd_header *>(stream + offset);
        if (cmd->size < sizeof(mtlb_cmd_header) || cmd->size > length - offset || (cmd->size & 7)) {
            note_error(fail(MTLB_ERROR_INVALID_ARGUMENT, "bad command size " + std::to_string(cmd->size)));
            return;
        }
        mtlb_result result = execute(cmd);
        if (result != MTLB_OK)
            note_error(result);
        offset += cmd->size;
    }
}

// The blit encoder, opened on demand. Copies end the render pass and run any
// pending clears first so they observe the cleared contents.
id<MTLBlitCommandEncoder> Replay::blit()
{
    if (!blit_) {
        end_compute();
        end_render();
        flush_clears(false);
        blit_ = [cb_ blitCommandEncoder];
        if (sync_needed_ && queue_->fence_pending) {
            [blit_ waitForFence:queue_->fence];
            queue_->fence_pending = false;
        }
        sync_needed_ = false;
    }
    return blit_;
}

// The compute encoder, opened on demand like the blit encoder.
id<MTLComputeCommandEncoder> Replay::compute()
{
    if (!compute_) {
        end_blit();
        end_render();
        flush_clears(false);
        // Dispatches may overlap; a barrier orders the ones around it.
        compute_ = [cb_ computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
        if (sync_needed_ && queue_->fence_pending) {
            [compute_ waitForFence:queue_->fence];
            queue_->fence_pending = false;
        }
        sync_needed_ = false;
        dirty_compute_ = kAll;
    }
    return compute_;
}

id<MTLRenderCommandEncoder> Replay::new_render_encoder(MTLRenderPassDescriptor *pass)
{
    end_blit();
    end_compute();
    queue_->render_passes.fetch_add(1, std::memory_order_relaxed);
    id<MTLRenderCommandEncoder> encoder = [cb_ renderCommandEncoderWithDescriptor:pass];
    if (sync_needed_ && queue_->fence_pending) {
        [encoder waitForFence:queue_->fence beforeStages:MTLRenderStageVertex];
        queue_->fence_pending = false;
    }
    sync_needed_ = false;
    return encoder;
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

bool Replay::is_bound(const PendingClear &clear) const
{
    if (clear.depth_stencil)
        return state_.depth == clear.target;
    return std::find(state_.targets, state_.targets + state_.num_targets, clear.target) != state_.targets + state_.num_targets;
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
    Target depth;
    mtlb_result result = resolve_target(cmd.depth, &depth);
    if (result != MTLB_OK)
        return result;
    // Binding the same targets again must not break the pass.
    if (cmd.count == state_.num_targets && std::equal(targets, targets + cmd.count, state_.targets)
        && depth == state_.depth && cmd.depth_flags == state_.depth_flags)
        return MTLB_OK;

    end_render();
    std::copy(targets, targets + cmd.count, state_.targets);
    std::fill(state_.targets + cmd.count, state_.targets + MTLB_MAX_RENDER_TARGETS, Target{});
    state_.num_targets = cmd.count;
    state_.depth = depth;
    state_.depth_flags = cmd.depth_flags;
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
    PendingClear clear;
    clear.target = target;
    std::copy_n(cmd.color, 4, clear.color);
    return add_clear(clear);
}

mtlb_result Replay::clear_dsv(const mtlb_cmd_clear_dsv &cmd)
{
    PendingClear clear;
    mtlb_result result = resolve_target(cmd.target, &clear.target);
    if (result != MTLB_OK)
        return result;
    if (!clear.target.texture)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "CLEAR_DSV without a texture");
    clear.depth_stencil = true;
    clear.aspects = cmd.flags;
    clear.depth = cmd.depth;
    clear.stencil = cmd.stencil;
    return add_clear(clear);
}

// A clear waits for the next pass that binds its view and becomes that pass's load action.
mtlb_result Replay::add_clear(const PendingClear &clear)
{
    if (render_) {
        // Draws may already have landed in the open pass. A clear of one of its
        // targets must come after them; a clear of any other view must not stay
        // pending while later draws in this list might read it. Either way the
        // pass ends, and a view outside it is cleared at once.
        const bool bound = is_bound(clear);
        end_render();
        if (!bound)
            return clear_only_pass(clear);
    }
    auto same_view = [&](const PendingClear &c) { return c.target == clear.target && c.depth_stencil == clear.depth_stencil; };
    PendingClear merged = clear;
    // A newer clear of the same view supersedes a pending one (for depth-stencil, plane by plane).
    for (const PendingClear &c : clears_) {
        if (!same_view(c) || !clear.depth_stencil)
            continue;
        if (!(clear.aspects & MTLB_CLEAR_DEPTH) && (c.aspects & MTLB_CLEAR_DEPTH)) {
            merged.aspects |= MTLB_CLEAR_DEPTH;
            merged.depth = c.depth;
        }
        if (!(clear.aspects & MTLB_CLEAR_STENCIL) && (c.aspects & MTLB_CLEAR_STENCIL)) {
            merged.aspects |= MTLB_CLEAR_STENCIL;
            merged.stencil = c.stencil;
        }
    }
    clears_.erase(std::remove_if(clears_.begin(), clears_.end(), same_view), clears_.end());
    clears_.push_back(merged);
    return MTLB_OK;
}

// Runs pending clears as passes of their own. With `keep_bound`, clears of the
// current targets stay pending so the next pass can fold them into load actions.
mtlb_result Replay::flush_clears(bool keep_bound)
{
    end_render();
    for (auto it = clears_.begin(); it != clears_.end();) {
        if (keep_bound && is_bound(*it)) {
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

// Sets up the depth and stencil attachments of `pass` on `view`, clearing the planes `clear` names.
void Replay::attach_depth(MTLRenderPassDescriptor *pass, const Target &t, id<MTLTexture> view, const PendingClear *clear)
{
    const bool has_stencil = view.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    const uint32_t aspects = clear ? clear->aspects : 0;
    MTLRenderPassDepthAttachmentDescriptor *da = pass.depthAttachment;
    da.texture = view;
    da.level = t.mip_level;
    da.slice = t.array_slice;
    da.storeAction = MTLStoreActionStore;
    da.loadAction = (aspects & MTLB_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
    if (aspects & MTLB_CLEAR_DEPTH)
        da.clearDepth = clear->depth;
    if (has_stencil) {
        MTLRenderPassStencilAttachmentDescriptor *sa = pass.stencilAttachment;
        sa.texture = view;
        sa.level = t.mip_level;
        sa.slice = t.array_slice;
        sa.storeAction = MTLStoreActionStore;
        sa.loadAction = (aspects & MTLB_CLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
        if (aspects & MTLB_CLEAR_STENCIL)
            sa.clearStencil = clear->stencil;
    }
}

mtlb_result Replay::clear_only_pass(const PendingClear &clear)
{
    id<MTLTexture> view = attachment_texture(clear.target.texture, clear.target.view_format);
    if (!view)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported attachment view format");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (clear.depth_stencil) {
        // Only the planes being cleared are attached; the other keeps its contents.
        const bool has_stencil = view.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        if (clear.aspects & MTLB_CLEAR_DEPTH) {
            MTLRenderPassDepthAttachmentDescriptor *da = pass.depthAttachment;
            da.texture = view;
            da.level = clear.target.mip_level;
            da.slice = clear.target.array_slice;
            da.loadAction = MTLLoadActionClear;
            da.storeAction = MTLStoreActionStore;
            da.clearDepth = clear.depth;
        }
        if (has_stencil && (clear.aspects & MTLB_CLEAR_STENCIL)) {
            MTLRenderPassStencilAttachmentDescriptor *sa = pass.stencilAttachment;
            sa.texture = view;
            sa.level = clear.target.mip_level;
            sa.slice = clear.target.array_slice;
            sa.loadAction = MTLLoadActionClear;
            sa.storeAction = MTLStoreActionStore;
            sa.clearStencil = clear.stencil;
        }
    } else {
        MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[0];
        ca.texture = view;
        ca.level = clear.target.mip_level;
        if (view.textureType == MTLTextureType3D)
            ca.depthPlane = clear.target.array_slice;
        else
            ca.slice = clear.target.array_slice;
        ca.loadAction = MTLLoadActionClear;
        ca.storeAction = MTLStoreActionStore;
        ca.clearColor = MTLClearColorMake(clear.color[0], clear.color[1], clear.color[2], clear.color[3]);
    }
    render_ = new_render_encoder(pass);
    end_render();
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
    pass_depth_format_ = pass_stencil_format_ = MTLPixelFormatInvalid;
    auto take_clear = [&](const Target &t, bool depth_stencil) -> std::optional<PendingClear> {
        auto it = std::find_if(clears_.begin(), clears_.end(),
                               [&](const PendingClear &c) { return c.target == t && c.depth_stencil == depth_stencil; });
        if (it == clears_.end())
            return std::nullopt;
        PendingClear clear = *it;
        clears_.erase(it);
        return clear;
    };
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
        if (view.textureType == MTLTextureType3D)
            ca.depthPlane = t.array_slice;
        else
            ca.slice = t.array_slice;
        ca.loadAction = MTLLoadActionLoad;
        ca.storeAction = MTLStoreActionStore;
        if (std::optional<PendingClear> clear = take_clear(t, false)) {
            ca.loadAction = MTLLoadActionClear;
            ca.clearColor = MTLClearColorMake(clear->color[0], clear->color[1], clear->color[2], clear->color[3]);
        }
        NSUInteger w = std::max<NSUInteger>(view.width >> t.mip_level, 1);
        NSUInteger h = std::max<NSUInteger>(view.height >> t.mip_level, 1);
        target_width_ = target_width_ ? std::min(target_width_, w) : w;
        target_height_ = target_height_ ? std::min(target_height_, h) : h;
    }
    if (state_.depth.texture) {
        const Target &t = state_.depth;
        id<MTLTexture> view = attachment_texture(t.texture, t.view_format);
        if (!view)
            return fail(MTLB_ERROR_UNSUPPORTED, "unsupported depth-stencil view format");
        const std::optional<PendingClear> clear = take_clear(t, true);
        attach_depth(pass, t, view, clear ? &*clear : nullptr);
        pass_depth_format_ = view.pixelFormat;
        pass_stencil_format_ = view.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? view.pixelFormat : MTLPixelFormatInvalid;
        NSUInteger w = std::max<NSUInteger>(view.width >> t.mip_level, 1);
        NSUInteger h = std::max<NSUInteger>(view.height >> t.mip_level, 1);
        target_width_ = target_width_ ? std::min(target_width_, w) : w;
        target_height_ = target_height_ ? std::min(target_height_, h) : h;
    }

    if (visibility_heap_ && visibility_heap_->results)
        pass.visibilityResultBuffer = visibility_heap_->results;
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
        id<MTLRenderPipelineState> pipeline_state = state_.pipeline->state_for(pass_depth_format_, pass_stencil_format_);
        if (!pipeline_state)
            return MTLB_ERROR_COMPILE_FAILED;
        [render_ setRenderPipelineState:pipeline_state];
        // Without a depth-stencil attachment, depth and stencil tests pass, as in D3D12.
        [render_ setDepthStencilState:pass_depth_format_ == MTLPixelFormatInvalid ? state_.pipeline->depth_stencil_off
                                                                                  : state_.pipeline->depth_stencil];
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
    if (dirty_ & kHeaps)
        bind_heaps(0);
    if ((dirty_ & kQuery) && query_.active)
        [render_ setVisibilityResultMode:query_.mode offset:uint64_t(query_.index) * 8];
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
    if (!state_.depth.texture
        && std::none_of(state_.targets, state_.targets + state_.num_targets, [](const Target &t) { return t.texture; })) {
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
    if ((uint64_t(cmd.start_index) + cmd.index_count) * state_.index_size > state_.index_view_size) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            std::fprintf(stderr, "d3d12-metal: indexed draw skipped, it reads past the index buffer view\n");
        return MTLB_OK;
    }
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
    // A depth-stencil texture is copied one plane at a time.
    MTLBlitOption options = MTLBlitOptionNone;
    if (texture->texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        options = r.plane == 0 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionStencilFromDepthStencil;
    if (to_buffer) {
        [enc copyFromTexture:texture->texture sourceSlice:r.array_slice sourceLevel:r.mip_level
                sourceOrigin:origin sourceSize:size toBuffer:buffer->buffer destinationOffset:r.buffer_offset
           destinationBytesPerRow:r.bytes_per_row destinationBytesPerImage:r.bytes_per_image options:options];
    } else {
        [enc copyFromBuffer:buffer->buffer sourceOffset:r.buffer_offset sourceBytesPerRow:r.bytes_per_row
            sourceBytesPerImage:r.bytes_per_image sourceSize:size toTexture:texture->texture
           destinationSlice:r.array_slice destinationLevel:r.mip_level destinationOrigin:origin options:options];
    }
    return MTLB_OK;
}

template <class T>
mtlb_result Replay::dispatch(const mtlb_cmd_header *header, mtlb_result (Replay::*handler)(const T &))
{
    if (header->size < sizeof(T)) {
        abort_stream_ = true;
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "command " + std::to_string(header->type) + " is too small");
    }
    return (this->*handler)(*reinterpret_cast<const T *>(header));
}

mtlb_result Replay::reset_state(const mtlb_cmd_reset_state &)
{
    sync_needed_ = !sync_disabled_;
    query_.active = false;
    visibility_heap_ = nullptr;
    // Whatever the previous list left (open pass, pending clears) completes first.
    mtlb_result result = flush_clears(false);
    if (result != MTLB_OK)
        return result;
    state_ = DrawState{};
    mark_all_dirty();
    dirty_compute_ = kAll;
    return MTLB_OK;
}

mtlb_result Replay::set_pipeline(const mtlb_cmd_set_pipeline &cmd)
{
    auto *pipeline = from_handle<Pipeline>(cmd.pipeline);
    if (!pipeline)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid pipeline handle");
    if (pipeline->compute) {
        if (pipeline != state_.compute_pipeline)
            dirty_compute_ |= kPipeline;
        state_.compute_pipeline = pipeline;
        return MTLB_OK;
    }
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
    state_.index_view_size = cmd.size;
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

mtlb_result Replay::set_compute_root_args(const mtlb_cmd_set_graphics_root_args &cmd)
{
    if (!array_fits<uint8_t>(cmd, cmd.data_size))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "root argument size exceeds the record");
    state_.compute_root_args = cmd.data;
    state_.compute_root_args_size = cmd.data_size;
    dirty_compute_ |= kRootArgs;
    return MTLB_OK;
}

mtlb_result Replay::set_descriptor_heaps(const mtlb_cmd_set_descriptor_heaps &cmd)
{
    state_.resource_heap = cmd.resource_heap;
    state_.sampler_heap = cmd.sampler_heap;
    dirty_ |= kHeaps;
    dirty_compute_ |= kHeaps;
    return MTLB_OK;
}

// Binds the descriptor heaps at their fixed bind points on the open encoder (render when
// `compute_stage` is 0, else compute).
void Replay::bind_heaps(uint32_t compute_stage)
{
    const uint64_t addresses[2] = {state_.resource_heap, state_.sampler_heap};
    const uint64_t points[2] = {kIRDescriptorHeapBindPoint, kIRSamplerHeapBindPoint};
    for (int i = 0; i < 2; ++i) {
        uint64_t offset = 0;
        Buffer *heap = addresses[i] ? find_buffer(queue_->device, addresses[i], &offset) : nullptr;
        if (!heap)
            continue;
        if (compute_stage) {
            [compute_ setBuffer:heap->buffer offset:offset atIndex:points[i]];
        } else {
            [render_ setVertexBuffer:heap->buffer offset:offset atIndex:points[i]];
            [render_ setFragmentBuffer:heap->buffer offset:offset atIndex:points[i]];
        }
    }
}

mtlb_result Replay::begin_query(const mtlb_cmd_query &cmd)
{
    auto *heap = from_handle<QueryHeap>(cmd.heap);
    if (!heap || cmd.index >= heap->count)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid query");
    if (cmd.type != MTLB_QUERY_OCCLUSION && cmd.type != MTLB_QUERY_BINARY_OCCLUSION)
        return MTLB_OK;  // statistics queries have no data to collect
    if (!heap->results)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "not an occlusion query heap");
    // The visibility buffer is a property of a render pass: a pass that has none is ended so that the draws
    // that follow open one that counts into this heap.
    if (render_ && visibility_heap_ != heap)
        end_render();
    visibility_heap_ = heap;
    query_.active = true;
    query_.index = cmd.index;
    query_.mode = cmd.type == MTLB_QUERY_BINARY_OCCLUSION ? MTLVisibilityResultModeBoolean : MTLVisibilityResultModeCounting;
    if (render_)
        [render_ setVisibilityResultMode:query_.mode offset:uint64_t(cmd.index) * 8];
    else
        dirty_ |= kQuery;
    return MTLB_OK;
}

mtlb_result Replay::end_query(const mtlb_cmd_query &cmd)
{
    auto *heap = from_handle<QueryHeap>(cmd.heap);
    if (!heap || cmd.index >= heap->count)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid query");
    if (cmd.type == MTLB_QUERY_TIMESTAMP)
        return timestamp(heap, cmd.index);
    if ((cmd.type == MTLB_QUERY_OCCLUSION || cmd.type == MTLB_QUERY_BINARY_OCCLUSION) && query_.active) {
        query_.active = false;
        if (render_)
            [render_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    }
    return MTLB_OK;
}

// A timestamp is the end of a compute pass of its own, after the work before it (on this hardware counters
// are sampled at the boundaries of encoders only).
mtlb_result Replay::timestamp(QueryHeap *heap, uint32_t index)
{
    if (!heap->samples)
        return MTLB_OK;
    end_blit();
    end_compute();
    mtlb_result result = flush_clears(false);
    if (result != MTLB_OK)
        return result;
    id<MTLComputePipelineState> kernel = internal_kernel(queue_->device, @"noop_kernel");
    if (!kernel)
        return MTLB_ERROR_COMPILE_FAILED;
    MTLComputePassDescriptor *pass = [MTLComputePassDescriptor computePassDescriptor];
    pass.sampleBufferAttachments[0].sampleBuffer = heap->samples;
    pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = MTLCounterDontSample;
    pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = index;
    id<MTLComputeCommandEncoder> enc = [cb_ computeCommandEncoderWithDescriptor:pass];
    if (queue_->fence_pending) {
        [enc waitForFence:queue_->fence];
        queue_->fence_pending = false;
    }
    sync_needed_ = false;
    [enc setComputePipelineState:kernel];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc updateFence:queue_->fence];
    queue_->fence_pending = true;
    [enc endEncoding];
    sync_needed_ = true;
    return MTLB_OK;
}

mtlb_result Replay::resolve_query(const mtlb_cmd_resolve_query &cmd)
{
    auto *heap = from_handle<QueryHeap>(cmd.heap);
    Buffer *dst = from_handle<Buffer>(cmd.dst);
    if (!heap || !dst || uint64_t(cmd.start) + cmd.count > heap->count)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid query resolve");
    const uint64_t element = cmd.type == MTLB_QUERY_PIPELINE_STATISTICS ? 88 : cmd.type == MTLB_QUERY_SO_STATISTICS ? 16 : 8;
    const uint64_t bytes = element * cmd.count;
    if (cmd.dst_offset > dst->size || bytes > dst->size - cmd.dst_offset)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "query results do not fit the destination");
    if (!bytes)
        return MTLB_OK;
    // Counter samples are not visible to a blit encoder in the same command buffer (it read zeros for the later
    // samples), so the work that wrote them is finished first.
    if (cmd.type == MTLB_QUERY_TIMESTAMP && heap->samples) {
        end_blit();
        end_compute();
        mtlb_result result = flush_clears(false);
        if (result != MTLB_OK)
            return result;
        id<MTLCommandBuffer> previous = cb_;
        commit_open(queue_);
        [previous waitUntilCompleted];
        cb_ = open_command_buffer(queue_);
        sync_needed_ = false;
    }
    id<MTLBlitCommandEncoder> enc = blit();
    if (element != 8 || cmd.type == MTLB_QUERY_PIPELINE_STATISTICS || cmd.type == MTLB_QUERY_SO_STATISTICS) {
        [enc fillBuffer:dst->buffer range:NSMakeRange(cmd.dst_offset, bytes) value:0];
    } else if (cmd.type == MTLB_QUERY_TIMESTAMP) {
        if (heap->samples)
            [enc resolveCounters:heap->samples inRange:NSMakeRange(cmd.start, cmd.count) destinationBuffer:dst->buffer
               destinationOffset:cmd.dst_offset];
        else
            [enc fillBuffer:dst->buffer range:NSMakeRange(cmd.dst_offset, bytes) value:0];
    } else if (heap->results) {
        [enc copyFromBuffer:heap->results sourceOffset:uint64_t(cmd.start) * 8 toBuffer:dst->buffer
          destinationOffset:cmd.dst_offset size:bytes];
    }
    return MTLB_OK;
}

mtlb_result Replay::marker(const mtlb_cmd_marker &cmd)
{
    if (!array_fits<char>(cmd, cmd.length))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "marker text exceeds the record");
    NSString *text = [[NSString alloc] initWithBytes:cmd.text length:strnlen(cmd.text, cmd.length) encoding:NSUTF8StringEncoding];
    if (!text)
        text = @"?";
    switch (cmd.kind) {
    case 0:
        [cb_ pushDebugGroup:text];
        ++debug_depth_;
        break;
    case 1:
        if (debug_depth_ > 0) {
            [cb_ popDebugGroup];
            --debug_depth_;
        }
        break;
    default:
        [cb_ pushDebugGroup:text];
        [cb_ popDebugGroup];
        break;
    }
    return MTLB_OK;
}

mtlb_result Replay::write_immediate(const mtlb_cmd_write_immediate &cmd)
{
    uint64_t offset = 0;
    Buffer *buffer = find_buffer(queue_->device, cmd.address, &offset);
    if (!buffer || (cmd.size != 4 && cmd.size != 8) || offset + cmd.size > buffer->size)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid immediate write");
    id<MTLComputePipelineState> kernel = internal_kernel(queue_->device, @"write_immediate");
    if (!kernel)
        return MTLB_ERROR_COMPILE_FAILED;
    // After everything recorded before it, and before whatever follows.
    sync_needed_ = !sync_disabled_;
    end_blit();
    end_render();
    id<MTLComputeCommandEncoder> enc = compute();
    const uint64_t where[2] = {offset, cmd.size};
    [enc setComputePipelineState:kernel];
    [enc setBuffer:buffer->buffer offset:0 atIndex:0];
    [enc setBytes:&cmd.value length:sizeof(cmd.value) atIndex:1];
    [enc setBytes:where length:sizeof(where) atIndex:2];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    dirty_compute_ = kAll;
    end_compute();
    sync_needed_ = !sync_disabled_;
    return MTLB_OK;
}

// A resolve is a render pass with no draws: the multisampled texture is loaded and stored with a resolve.
mtlb_result Replay::resolve(const mtlb_cmd_resolve &cmd)
{
    Texture *dst = from_handle<Texture>(cmd.dst), *src = from_handle<Texture>(cmd.src);
    if (!dst || !src || src->texture.sampleCount < 2 || dst->texture.sampleCount != 1)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "resolve needs a multisampled source and a single-sampled destination");
    // Earlier clears and draws land first.
    end_blit();
    end_compute();
    mtlb_result result = flush_clears(false);
    if (result != MTLB_OK)
        return result;
    id<MTLTexture> source = attachment_texture(src, cmd.format == static_cast<uint32_t>(src->format) ? 0 : cmd.format);
    id<MTLTexture> destination = attachment_texture(dst, cmd.format == static_cast<uint32_t>(dst->format) ? 0 : cmd.format);
    if (!source || !destination)
        return fail(MTLB_ERROR_UNSUPPORTED, "unsupported resolve format");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[0];
    ca.texture = source;
    ca.level = cmd.src_mip;
    ca.slice = cmd.src_slice;
    ca.resolveTexture = destination;
    ca.resolveLevel = cmd.dst_mip;
    ca.resolveSlice = cmd.dst_slice;
    ca.loadAction = MTLLoadActionLoad;
    ca.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    render_ = new_render_encoder(pass);
    end_render();
    return MTLB_OK;
}

// The compute encoder with the application's compute pipeline, root arguments and descriptor heaps bound.
id<MTLComputeCommandEncoder> Replay::prepare_dispatch()
{
    if (!state_.compute_pipeline) {
        fail(MTLB_ERROR_INVALID_ARGUMENT, "dispatch without a compute pipeline");
        return nil;
    }
    id<MTLComputeCommandEncoder> enc = compute();
    if (!enc) {
        fail(MTLB_ERROR_DEVICE, "computeCommandEncoder failed");
        return nil;
    }
    if (dirty_compute_ & kPipeline)
        [enc setComputePipelineState:state_.compute_pipeline->compute];
    if ((dirty_compute_ & kRootArgs) && state_.compute_root_args_size)
        [enc setBytes:state_.compute_root_args length:state_.compute_root_args_size atIndex:kIRArgumentBufferBindPoint];
    if (dirty_compute_ & kHeaps)
        bind_heaps(1);
    dirty_compute_ = 0;
    return enc;
}

mtlb_result Replay::dispatch_compute(const mtlb_cmd_dispatch &cmd)
{
    if (!state_.compute_pipeline)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "dispatch without a compute pipeline");
    if (!cmd.x || !cmd.y || !cmd.z)
        return MTLB_OK;  // an empty dispatch does nothing (Metal rejects it)
    id<MTLComputeCommandEncoder> enc = prepare_dispatch();
    if (!enc)
        return MTLB_ERROR_DEVICE;
    [enc dispatchThreadgroups:MTLSizeMake(cmd.x, cmd.y, cmd.z) threadsPerThreadgroup:state_.compute_pipeline->threadgroup_size];
    return MTLB_OK;
}

// ExecuteIndirect. A command with nothing but its action, and no count, is read from the application's buffer
// as it is (Metal's indirect argument layouts equal D3D12's). Otherwise a kernel first turns every command into
// a record of its own (root arguments and vertex buffer table with the command's changes, action arguments
// zeroed past the count), and the encoder binds one record per command. The render pass or compute encoder of
// the kernel is separate from the draws it feeds.
mtlb_result Replay::execute_indirect(const mtlb_cmd_execute_indirect &cmd)
{
    if (!array_fits<mtlb_indirect_arg>(cmd, cmd.num_args) || cmd.stride == 0 || cmd.action < 1 || cmd.action > 3)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid indirect command");
    if (cmd.max_count == 0)
        return MTLB_OK;
    uint64_t arg_offset = 0, count_offset = 0;
    Buffer *arguments = find_buffer(queue_->device, cmd.arg_address, &arg_offset);
    Buffer *count = cmd.count_address ? find_buffer(queue_->device, cmd.count_address, &count_offset) : nullptr;
    if (!arguments || (cmd.count_address && !count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indirect argument or count buffer not found");
    // The commands must lie inside the argument buffer, and so must what the translation writes into the records.
    const uint32_t action_bytes = cmd.action == MTLB_INDIRECT_DRAW_INDEXED ? 20 : cmd.action == MTLB_INDIRECT_DRAW ? 16 : 12;
    const uint64_t first_end = arg_offset + cmd.action_src_offset + action_bytes;
    if (first_end > arguments->size || cmd.stride % 4)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indirect commands do not fit the argument buffer");
    const uint32_t max_count = static_cast<uint32_t>(
        std::min<uint64_t>(cmd.max_count, (arguments->size - first_end) / cmd.stride + 1));
    const bool graphics = cmd.action != MTLB_INDIRECT_DISPATCH;
    const bool direct = cmd.num_args == 0 && !cmd.count_address;
    if (graphics && !state_.pipeline)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indirect draw without a pipeline");

    id<MTLBuffer> scratch = nil;
    uint32_t record_size = 0, vb_offset = 0, action_offset = 0, root_size = 0;
    if (!direct) {
        const uint8_t *root = graphics ? state_.root_args : state_.compute_root_args;
        root_size = graphics ? state_.root_args_size : state_.compute_root_args_size;
        for (uint32_t k = 0; k < cmd.num_args; ++k) {
            const mtlb_indirect_arg &arg = cmd.args[k];
            const uint64_t size = arg.type == MTLB_INDIRECT_ARG_CONSTANT ? arg.size
                                  : arg.type == MTLB_INDIRECT_ARG_POINTER ? 8
                                  : arg.type == MTLB_INDIRECT_ARG_VERTEX_BUFFER ? 16 : 4;
            const uint64_t source_size = arg.type == MTLB_INDIRECT_ARG_COMMAND_INDEX ? 0 : size;
            const bool to_root = arg.type != MTLB_INDIRECT_ARG_VERTEX_BUFFER;
            if (arg.type < MTLB_INDIRECT_ARG_CONSTANT || arg.type > MTLB_INDIRECT_ARG_COMMAND_INDEX
                || uint64_t(arg.src_offset) + source_size > cmd.stride
                || (to_root ? uint64_t(arg.dst_offset) + size > root_size : arg.dst_offset >= MTLB_MAX_VERTEX_BUFFERS))
                return fail(MTLB_ERROR_INVALID_ARGUMENT, "indirect argument outside its destination");
        }
        vb_offset = (root_size + 15) & ~15u;
        action_offset = vb_offset + sizeof(state_.vertex_buffers);  // 496 bytes, a multiple of 16
        record_size = (action_offset + 32 + 15) & ~15u;
        scratch = [queue_->device->device newBufferWithLength:uint64_t(record_size) * max_count
                                                      options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked];
        if (!scratch)
            return fail(MTLB_ERROR_OUT_OF_MEMORY, "indirect scratch buffer");
        // The scratch buffer lives until the command buffer has run.
        [cb_ addCompletedHandler:^(id<MTLCommandBuffer>) { (void)scratch; }];

        id<MTLComputePipelineState> kernel = internal_kernel(queue_->device, @"translate_indirect");
        if (!kernel)
            return MTLB_ERROR_COMPILE_FAILED;
        id<MTLComputeCommandEncoder> enc = compute();
        struct Params {
            uint32_t action, max_count, stride, action_src, num_args, root_size, vb_offset, action_offset, record_size,
                has_count, pad[2];
        } params = {cmd.action, max_count, cmd.stride, cmd.action_src_offset, cmd.num_args, root_size, vb_offset,
                    action_offset, record_size, cmd.count_address ? 1u : 0u, {}};
        const uint64_t base[2] = {arg_offset, count_offset};
        const uint8_t zeros[16] = {};
        [enc setComputePipelineState:kernel];
        [enc setBuffer:arguments->buffer offset:0 atIndex:0];
        if (cmd.num_args)
            [enc setBytes:cmd.args length:cmd.num_args * sizeof(mtlb_indirect_arg) atIndex:1];
        else
            [enc setBytes:zeros length:sizeof(zeros) atIndex:1];
        [enc setBytes:root_size ? static_cast<const void *>(root) : zeros length:std::max<uint32_t>(root_size, 16) atIndex:2];
        [enc setBytes:state_.vertex_buffers length:sizeof(state_.vertex_buffers) atIndex:3];
        [enc setBuffer:scratch offset:0 atIndex:4];
        [enc setBytes:&params length:sizeof(params) atIndex:5];
        [enc setBytes:base length:sizeof(base) atIndex:6];
        [enc setBuffer:(count ? count : arguments)->buffer offset:0 atIndex:7];
        [enc dispatchThreads:MTLSizeMake(max_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(max_count, 64), 1, 1)];
        dirty_compute_ = kAll;
        if (graphics) {
            end_compute();       // the draws below wait for the kernel
            sync_needed_ = true;
        } else {
            [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
    }

    auto source = [&](uint32_t i, uint64_t *offset) -> id<MTLBuffer> {
        if (direct) {
            *offset = arg_offset + uint64_t(i) * cmd.stride + cmd.action_src_offset;
            return arguments->buffer;
        }
        *offset = uint64_t(i) * record_size + action_offset;
        return scratch;
    };

    if (!graphics) {
        id<MTLComputeCommandEncoder> enc = prepare_dispatch();
        if (!enc)
            return MTLB_ERROR_DEVICE;
        for (uint32_t i = 0; i < max_count; ++i) {
            uint64_t offset;
            id<MTLBuffer> buffer = source(i, &offset);
            if (!direct && root_size)
                [enc setBuffer:scratch offset:uint64_t(i) * record_size atIndex:kIRArgumentBufferBindPoint];
            [enc dispatchThreadgroupsWithIndirectBuffer:buffer indirectBufferOffset:offset
                                  threadsPerThreadgroup:state_.compute_pipeline->threadgroup_size];
        }
        dirty_compute_ |= kRootArgs;  // the application's arguments are bound again at its next dispatch
        return MTLB_OK;
    }

    bool ready;
    mtlb_result result = begin_draw(&ready);
    if (result != MTLB_OK || !ready)
        return result;
    if (cmd.action == MTLB_INDIRECT_DRAW_INDEXED && (!state_.index_buffer || (state_.index_size != 2 && state_.index_size != 4)))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "indexed indirect draw without a valid index buffer");
    for (uint32_t i = 0; i < max_count; ++i) {
        uint64_t offset;
        id<MTLBuffer> buffer = source(i, &offset);
        if (!direct) {
            if (root_size) {
                [render_ setVertexBuffer:scratch offset:uint64_t(i) * record_size atIndex:kIRArgumentBufferBindPoint];
                [render_ setFragmentBuffer:scratch offset:uint64_t(i) * record_size atIndex:kIRArgumentBufferBindPoint];
            }
            [render_ setVertexBuffer:scratch offset:uint64_t(i) * record_size + vb_offset atIndex:kIRVertexBufferBindPoint];
        }
        if (cmd.action == MTLB_INDIRECT_DRAW)
            IRRuntimeDrawPrimitives(render_, to_primitive_type(state_.topology), buffer, offset);
        else
            IRRuntimeDrawIndexedPrimitives(render_, to_primitive_type(state_.topology),
                                           state_.index_size == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32,
                                           state_.index_buffer, state_.index_offset, buffer, offset);
    }
    if (!direct)
        dirty_ |= kRootArgs | kVertexBuffers;  // the application's state is bound again at its next draw
    return MTLB_OK;
}

// Barriers synchronise the work before them with the work after. Between encoders that is the fence; inside
// an open compute encoder a memory barrier orders the dispatches around it. A render pass cannot be waited
// on from the inside on this hardware (fragment work is not a stage a barrier can follow), so a barrier ends
// it: its writes are stored and the next pass loads them.
mtlb_result Replay::barrier(const mtlb_cmd_barrier &cmd)
{
    if (!array_fits<mtlb_barrier>(cmd, cmd.count))
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "barrier count exceeds the record");
    if (sync_disabled_)
        return MTLB_OK;
    sync_needed_ = true;
    end_blit();
    end_render();
    if (compute_)
        [compute_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    return MTLB_OK;
}

// Our own kernels share the compute encoder with the application's dispatches and rebind the same slots.
mtlb_result Replay::clear_buffer(const mtlb_cmd_clear_buffer &cmd)
{
    Buffer *buffer = from_handle<Buffer>(cmd.buffer);
    if (!buffer || cmd.pattern_size == 0 || cmd.pattern_size > 16 || cmd.offset > buffer->size
        || cmd.size > buffer->size - cmd.offset)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid buffer clear");
    const uint32_t elements = static_cast<uint32_t>(std::min<uint64_t>(cmd.size / cmd.pattern_size, UINT32_MAX));
    if (!elements)
        return MTLB_OK;
    id<MTLComputePipelineState> kernel = internal_kernel(queue_->device, @"clear_buffer");
    if (!kernel)
        return MTLB_ERROR_COMPILE_FAILED;
    id<MTLComputeCommandEncoder> enc = compute();
    const uint32_t params[2] = {elements, cmd.pattern_size};
    const uint64_t base = cmd.offset;
    [enc setComputePipelineState:kernel];
    [enc setBuffer:buffer->buffer offset:0 atIndex:0];
    [enc setBytes:cmd.pattern length:sizeof(cmd.pattern) atIndex:1];
    [enc setBytes:params length:sizeof(params) atIndex:2];
    [enc setBytes:&base length:sizeof(base) atIndex:3];
    [enc dispatchThreads:MTLSizeMake(elements, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(elements, 256), 1, 1)];
    dirty_compute_ = kAll;
    return MTLB_OK;
}

mtlb_result Replay::clear_texture_uav(const mtlb_cmd_clear_texture_uav &cmd)
{
    Texture *texture = from_handle<Texture>(cmd.texture);
    if (!texture)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid texture clear");
    id<MTLTexture> view = texture_view_object(texture, &cmd.view);
    if (!view)
        return MTLB_ERROR_UNSUPPORTED;
    const char *dimension = view.textureType == MTLTextureType3D ? "3d" : view.textureType == MTLTextureType2DArray ? "2da" : "2d";
    if (view.textureType != MTLTextureType3D && view.textureType != MTLTextureType2DArray && view.textureType != MTLTextureType2D)
        return fail(MTLB_ERROR_UNSUPPORTED, "cannot clear this kind of texture view");
    const char *suffix = cmd.kind == MTLB_CLEAR_UINT ? "u" : cmd.kind == MTLB_CLEAR_SINT ? "i" : "f";
    id<MTLComputePipelineState> kernel =
        internal_kernel(queue_->device, [NSString stringWithFormat:@"clear%s_%s", dimension, suffix]);
    if (!kernel)
        return MTLB_ERROR_COMPILE_FAILED;
    const uint32_t width = cmd.width ? cmd.width : static_cast<uint32_t>(view.width) - std::min<uint32_t>(cmd.x, static_cast<uint32_t>(view.width));
    const uint32_t height = cmd.height ? cmd.height : static_cast<uint32_t>(view.height) - std::min<uint32_t>(cmd.y, static_cast<uint32_t>(view.height));
    const uint32_t region[4] = {cmd.x, cmd.y, width, height};
    const NSUInteger depth = view.textureType == MTLTextureType3D ? view.depth
                             : view.textureType == MTLTextureType2DArray ? view.arrayLength : 1;
    if (!width || !height)
        return MTLB_OK;
    id<MTLComputeCommandEncoder> enc = compute();
    [enc setComputePipelineState:kernel];
    [enc setTexture:view atIndex:0];
    [enc setBytes:cmd.value length:sizeof(cmd.value) atIndex:0];
    [enc setBytes:region length:sizeof(region) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(width, height, depth) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    dirty_compute_ = kAll;
    return MTLB_OK;
}

// A texture viewed in another pixel format (a copy between formats of one typeless family).
static id<MTLTexture> reinterpreted(Texture *texture, MTLPixelFormat format)
{
    if (texture->texture.pixelFormat == format)
        return texture->texture;
    std::lock_guard<std::mutex> lock(texture->views_mutex);
    // Pixel formats occupy keys of their own, apart from the render target views' mtlb formats.
    auto &view = texture->views[0x80000000u | static_cast<uint32_t>(format)];
    if (!view)
        view = [texture->texture newTextureViewWithPixelFormat:format];
    return view;
}

mtlb_result Replay::copy_texture_texture(const mtlb_cmd_copy_texture_texture &cmd)
{
    Texture *dst = from_handle<Texture>(cmd.dst), *src = from_handle<Texture>(cmd.src);
    if (!dst || !src)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "invalid texture copy");
    id<MTLBlitCommandEncoder> enc = blit();
    id<MTLTexture> source = reinterpreted(src, dst->texture.pixelFormat);
    if (!source)
        return fail(MTLB_ERROR_UNSUPPORTED, "texture copy between incompatible formats");
    if (cmd.whole) {
        const NSUInteger slices = std::max<NSUInteger>(dst->texture.arrayLength, 1);
        [enc copyFromTexture:source sourceSlice:0 sourceLevel:0 toTexture:dst->texture destinationSlice:0
            destinationLevel:0 sliceCount:slices levelCount:dst->texture.mipmapLevelCount];
        return MTLB_OK;
    }
    [enc copyFromTexture:source sourceSlice:cmd.src_slice sourceLevel:cmd.src_mip
            sourceOrigin:MTLOriginMake(cmd.src_x, cmd.src_y, cmd.src_z)
              sourceSize:MTLSizeMake(cmd.width, cmd.height, cmd.depth)
               toTexture:dst->texture destinationSlice:cmd.dst_slice destinationLevel:cmd.dst_mip
       destinationOrigin:MTLOriginMake(cmd.dst_x, cmd.dst_y, cmd.dst_z)];
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
    case MTLB_CMD_CLEAR_DSV: return dispatch(header, &Replay::clear_dsv);
    case MTLB_CMD_SET_PIPELINE: return dispatch(header, &Replay::set_pipeline);
    case MTLB_CMD_SET_VIEWPORTS: return dispatch(header, &Replay::set_viewports);
    case MTLB_CMD_SET_SCISSORS: return dispatch(header, &Replay::set_scissors);
    case MTLB_CMD_SET_TOPOLOGY: return dispatch(header, &Replay::set_topology);
    case MTLB_CMD_SET_VERTEX_BUFFERS: return dispatch(header, &Replay::set_vertex_buffers);
    case MTLB_CMD_SET_INDEX_BUFFER: return dispatch(header, &Replay::set_index_buffer);
    case MTLB_CMD_SET_GRAPHICS_ROOT_ARGS: return dispatch(header, &Replay::set_root_args);
    case MTLB_CMD_SET_BLEND_FACTOR: return dispatch(header, &Replay::set_blend_factor);
    case MTLB_CMD_SET_STENCIL_REF: return dispatch(header, &Replay::set_stencil_ref);
    case MTLB_CMD_SET_COMPUTE_ROOT_ARGS: return dispatch(header, &Replay::set_compute_root_args);
    case MTLB_CMD_SET_DESCRIPTOR_HEAPS: return dispatch(header, &Replay::set_descriptor_heaps);
    case MTLB_CMD_DISPATCH: return dispatch(header, &Replay::dispatch_compute);
    case MTLB_CMD_BARRIER: return dispatch(header, &Replay::barrier);
    case MTLB_CMD_RESOLVE: return dispatch(header, &Replay::resolve);
    case MTLB_CMD_BEGIN_QUERY: return dispatch(header, &Replay::begin_query);
    case MTLB_CMD_END_QUERY: return dispatch(header, &Replay::end_query);
    case MTLB_CMD_RESOLVE_QUERY: return dispatch(header, &Replay::resolve_query);
    case MTLB_CMD_MARKER: return dispatch(header, &Replay::marker);
    case MTLB_CMD_WRITE_IMMEDIATE: return dispatch(header, &Replay::write_immediate);
    case MTLB_CMD_EXECUTE_INDIRECT: return dispatch(header, &Replay::execute_indirect);
    case MTLB_CMD_CLEAR_BUFFER: return dispatch(header, &Replay::clear_buffer);
    case MTLB_CMD_CLEAR_TEXTURE_UAV: return dispatch(header, &Replay::clear_texture_uav);
    case MTLB_CMD_COPY_TEXTURE_TEXTURE: return dispatch(header, &Replay::copy_texture_texture);
    case MTLB_CMD_DRAW: return dispatch(header, &Replay::draw);
    case MTLB_CMD_DRAW_INDEXED: return dispatch(header, &Replay::draw_indexed);
    case MTLB_CMD_COPY_BUFFER: return dispatch(header, &Replay::copy_buffer);
    case MTLB_CMD_COPY_TEXTURE_TO_BUFFER: return dispatch(header, &Replay::copy_texture_to_buffer);
    case MTLB_CMD_COPY_BUFFER_TO_TEXTURE: return dispatch(header, &Replay::copy_buffer_to_texture);
    default:
        abort_stream_ = true;
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "unknown command type " + std::to_string(header->type));
    }
}

// The queue's open command buffer, created on first use. A wait is encoded into
// it and stays there, ahead of the work of the next submit or signal, which
// commit it.
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
    for (; queue->debug_depth > 0; --queue->debug_depth)
        [queue->open popDebugGroup];
    commit_residency(queue->device);
    [queue->open commit];
    queue->open = nil;
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
    queue->fence = [device->device newFence];
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
    // Every span runs, whatever happened to the ones before it.
    for (uint32_t i = 0; i < count; ++i) {
        if (!spans[i].data && spans[i].size)
            replay.note_span_error();
        else
            replay.run(spans[i].data, spans[i].size);
    }
    replay.finish();
    commit_open(queue);
    return replay.error() == MTLB_OK ? MTLB_OK : fail(replay.error(), replay.error_message());
}

uint64_t mtlb_queue_render_pass_count(mtlb_queue handle)
{
    Queue *queue = from_handle<Queue>(handle);
    return queue ? queue->render_passes.load() : 0;
}

// Appends the presentation to the open buffer and commits it.
mtlb_result mtlb_queue_present(mtlb_queue handle, mtlb_swapchain swapchain_handle, mtlb_texture texture_handle,
                               uint32_t sync_interval)
{
    Queue *queue = from_handle<Queue>(handle);
    Swapchain *swapchain = from_handle<Swapchain>(swapchain_handle);
    Texture *texture = from_handle<Texture>(texture_handle);
    if (!queue || !swapchain || !texture)
        return MTLB_ERROR_INVALID_ARGUMENT;
    if (texture->texture.sampleCount != 1)
        return fail(MTLB_ERROR_INVALID_ARGUMENT, "cannot present a multisampled texture");
    // A frame with no drawable (nothing to show) is dropped; the queue's other work still runs.
    const Drawable drawable = acquire_drawable(swapchain, sync_interval);
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (drawable.drawable)
        encode_present(queue, open_command_buffer(queue), swapchain, texture, drawable);
    commit_open(queue);
    return MTLB_OK;
}

// Debug groups on the queue's open command buffer.
mtlb_result mtlb_queue_marker(mtlb_queue handle, uint32_t kind, const char *text)
{
    Queue *queue = from_handle<Queue>(handle);
    if (!queue)
        return MTLB_ERROR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(queue->mutex);
    id<MTLCommandBuffer> cb = open_command_buffer(queue);
    if (kind == 0) {
        [cb pushDebugGroup:text ? @(text) : @"?"];
        ++queue->debug_depth;
    } else if (queue->debug_depth > 0) {
        [cb popDebugGroup];
        --queue->debug_depth;
    }
    return MTLB_OK;
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
