#include "d3d12/command_list.h"

#include <algorithm>
#include <cstring>

#include "d3d12/clear_value.h"
#include "d3d12/command_allocator.h"
#include "d3d12/device.h"
#include "d3d12/formats.h"
#include "d3d12/pipeline_state.h"
#include "d3d12/resource.h"

namespace d3d12m {

namespace {

const RenderTargetDescriptor &rtv_from_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    return *reinterpret_cast<const RenderTargetDescriptor *>(handle.ptr);
}

mtlb_render_target to_render_target(const RenderTargetDescriptor &rtv)
{
    return {rtv.texture, rtv.view_format, rtv.mip_level, rtv.array_slice, 0};
}

} // namespace

HRESULT CommandList::create(Device *device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
                            ID3D12PipelineState *initial_state, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!allocator)
        return E_INVALIDARG;
    if (!supported_list_type(type))
        return E_INVALIDARG;
    auto *list = new CommandList(device, type);
    list->reset_state();
    if (initial_state)
        list->SetPipelineState(initial_state);
    return hand_out(list, riid, out);
}

CommandList::~CommandList()
{
    safe_release(graphics_.signature);
    safe_release(compute_.signature);
}

template <typename T>
T *CommandList::append(mtlb_cmd_type type, size_t extra_bytes)
{
    const size_t size = mtlb_cmd_align(static_cast<uint32_t>(sizeof(T) + extra_bytes));
    const size_t at = stream_.size();
    stream_.resize(at + size);  // zero-filled
    auto *cmd = reinterpret_cast<T *>(stream_.data() + at);
    cmd->header = {static_cast<uint32_t>(type), static_cast<uint32_t>(size)};
    return cmd;
}

void CommandList::reset_state()
{
    stream_.clear();
    has_graphics_pipeline_ = has_compute_pipeline_ = false;
    for (RootState *state : {&graphics_, &compute_}) {
        safe_release(state->signature);
        state->args.clear();
        state->dirty = false;
    }
    // Replay state must not leak in from the previous list of the same submit.
    append<mtlb_cmd_reset_state>(MTLB_CMD_RESET_STATE);
}

// Gets the stream ready for a draw: the root arguments must be current. Returns
// false (after logging) when the draw cannot be recorded.
bool CommandList::prepare_draw()
{
    if (closed_ || !has_graphics_pipeline_ || !graphics_.signature) {
        D3D12M_LOG("draw skipped: needs an open list, a graphics pipeline and a root signature");
        return false;
    }
    flush_root_args(graphics_, MTLB_CMD_SET_GRAPHICS_ROOT_ARGS);
    return true;
}

// Sends the root arguments if they changed since the last draw or dispatch.
void CommandList::flush_root_args(RootState &state, mtlb_cmd_type type)
{
    if (state.dirty && !state.args.empty()) {
        // Graphics and compute arguments share the record layout.
        auto *cmd = append<mtlb_cmd_set_graphics_root_args>(type, state.args.size());
        cmd->data_size = static_cast<uint32_t>(state.args.size());
        std::memcpy(cmd->data, state.args.data(), state.args.size());
    }
    state.dirty = false;
}

// ---- Lifetime ---------------------------------------------------------------

HRESULT CommandList::Close()
{
    if (closed_)
        return E_FAIL;
    closed_ = true;
    return S_OK;
}

HRESULT CommandList::Reset(ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state)
{
    if (!closed_ || !allocator)
        return E_FAIL;
    reset_state();
    closed_ = false;
    if (initial_state)
        SetPipelineState(initial_state);
    return S_OK;
}

// ---- State ------------------------------------------------------------------

void CommandList::SetPipelineState(ID3D12PipelineState *pso)
{
    auto *state = ours<PipelineState>(pso);
    if (!state) {
        if (pso)
            D3D12M_LOG("SetPipelineState: the pipeline state is not from this layer");
        return;
    }
    (state->is_compute() ? has_compute_pipeline_ : has_graphics_pipeline_) = true;
    append<mtlb_cmd_set_pipeline>(MTLB_CMD_SET_PIPELINE)->pipeline = state->handle();
}

void CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        append<mtlb_cmd_set_topology>(MTLB_CMD_SET_TOPOLOGY)->topology = topology;  // values match mtlb_topology
        break;
    default:
        D3D12M_LOG("unsupported primitive topology %d", static_cast<int>(topology));
    }
}

void CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports)
{
    if (!viewports || count > MTLB_MAX_VIEWPORTS)
        return;
    auto *cmd = append<mtlb_cmd_set_viewports>(MTLB_CMD_SET_VIEWPORTS, count * sizeof(mtlb_viewport));
    cmd->count = count;
    for (UINT i = 0; i < count; ++i) {
        const D3D12_VIEWPORT &v = viewports[i];
        cmd->viewports[i] = {v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth};
    }
}

void CommandList::RSSetScissorRects(UINT count, const D3D12_RECT *rects)
{
    if (!rects || count > MTLB_MAX_VIEWPORTS)
        return;
    auto *cmd = append<mtlb_cmd_set_scissors>(MTLB_CMD_SET_SCISSORS, count * sizeof(mtlb_rect));
    cmd->count = count;
    for (UINT i = 0; i < count; ++i)
        cmd->rects[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
}

void CommandList::OMSetBlendFactor(const FLOAT factor[4])
{
    auto *cmd = append<mtlb_cmd_set_blend_factor>(MTLB_CMD_SET_BLEND_FACTOR);
    if (factor)
        std::copy_n(factor, 4, cmd->factor);
    else
        std::fill_n(cmd->factor, 4, 1.0f);
}

void CommandList::OMSetStencilRef(UINT ref)
{
    append<mtlb_cmd_set_stencil_ref>(MTLB_CMD_SET_STENCIL_REF)->ref = ref;
}

void CommandList::IASetVertexBuffers(UINT start_slot, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
{
    if (count == 0 || start_slot + count > MTLB_MAX_VERTEX_BUFFERS)
        return;
    auto *cmd = append<mtlb_cmd_set_vertex_buffers>(MTLB_CMD_SET_VERTEX_BUFFERS, count * sizeof(mtlb_vertex_buffer));
    cmd->start_slot = start_slot;
    cmd->count = count;
    for (UINT i = 0; i < count && views; ++i)
        cmd->buffers[i] = {views[i].BufferLocation, views[i].SizeInBytes, views[i].StrideInBytes};
}

void CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view)
{
    auto *cmd = append<mtlb_cmd_set_index_buffer>(MTLB_CMD_SET_INDEX_BUFFER);
    if (!view)
        return;
    if (view->Format != DXGI_FORMAT_R16_UINT && view->Format != DXGI_FORMAT_R32_UINT) {
        D3D12M_LOG("unsupported index buffer format %d", static_cast<int>(view->Format));
        return;
    }
    cmd->gpu_address = view->BufferLocation;
    cmd->size = view->SizeInBytes;
    cmd->index_size = view->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
}

void CommandList::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs,
                                     BOOL single_handle_to_range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
{
    if (count > MTLB_MAX_RENDER_TARGETS || (count && !rtvs))
        return;
    if (dsv)
        D3D12M_STUB_LOG();  // depth-stencil views are not implemented
    auto *cmd = append<mtlb_cmd_set_render_targets>(MTLB_CMD_SET_RENDER_TARGETS, count * sizeof(mtlb_render_target));
    cmd->count = count;
    for (UINT i = 0; i < count; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = single_handle_to_range
                                                 ? D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + i * kDescriptorSize}
                                                 : rtvs[i];
        cmd->targets[i] = to_render_target(rtv_from_handle(handle));
    }
}

void CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT num_rects,
                                        const D3D12_RECT *)
{
    const RenderTargetDescriptor &rtv = rtv_from_handle(view);
    if (!rtv.texture || !color)
        return;
    if (num_rects)
        D3D12M_LOG("ClearRenderTargetView: clear rectangles are ignored, the whole view is cleared");
    auto *cmd = append<mtlb_cmd_clear_rtv>(MTLB_CMD_CLEAR_RTV);
    cmd->target = to_render_target(rtv);
    std::copy_n(color, 4, cmd->color);
}

// ---- Root arguments ---------------------------------------------------------

void CommandList::set_root_signature(RootState &state, ID3D12RootSignature *signature, const char *what)
{
    auto *rs = ours<RootSignature>(signature);
    if (signature && !rs) {
        D3D12M_LOG("%s: the root signature is not from this layer", what);
        return;
    }
    if (rs == state.signature)
        return;  // the same signature keeps its bindings
    if (rs)
        rs->AddRef();
    safe_release(state.signature);
    state.signature = rs;
    // Changing the root signature invalidates all root arguments.
    state.args.assign(rs ? rs->argument_buffer_size() : 0, 0);
    if (rs)
        rs->init_arguments(state.args.data());
    state.dirty = true;
}

void CommandList::SetGraphicsRootSignature(ID3D12RootSignature *signature)
{
    set_root_signature(graphics_, signature, "SetGraphicsRootSignature");
}

void CommandList::SetComputeRootSignature(ID3D12RootSignature *signature)
{
    set_root_signature(compute_, signature, "SetComputeRootSignature");
}

const RootSignature::Slot *CommandList::find_slot(RootState &state, UINT index, D3D12_ROOT_PARAMETER_TYPE type)
{
    if (!state.signature || index >= state.signature->slots().size() || state.signature->slots()[index].type != type) {
        D3D12M_LOG("root parameter %u is not set up for this kind of argument", index);
        return nullptr;
    }
    return &state.signature->slots()[index];
}

void CommandList::set_root_address(RootState &state, UINT index, D3D12_ROOT_PARAMETER_TYPE type, uint64_t address)
{
    if (const RootSignature::Slot *slot = find_slot(state, index, type)) {
        std::memcpy(state.args.data() + slot->offset, &address, sizeof(address));
        state.dirty = true;
    }
}

void CommandList::set_root_constants(RootState &state, UINT index, UINT count, const void *data, UINT dest_offset)
{
    const RootSignature::Slot *slot = find_slot(state, index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    if (!slot || !data)
        return;
    if ((uint64_t(dest_offset) + count) * 4 > slot->size) {
        D3D12M_LOG("root constants for parameter %u exceed its %u bytes", index, slot->size);
        return;
    }
    std::memcpy(state.args.data() + slot->offset + dest_offset * 4, data, count * 4);
    state.dirty = true;
}

void CommandList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    set_root_address(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, base.ptr);
}

void CommandList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_CBV, address);
}

void CommandList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_SRV, address);
}

void CommandList::SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_UAV, address);
}

void CommandList::SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT dest_offset)
{
    set_root_constants(graphics_, index, 1, &value, dest_offset);
}

void CommandList::SetGraphicsRoot32BitConstants(UINT index, UINT count, const void *data, UINT dest_offset)
{
    set_root_constants(graphics_, index, count, data, dest_offset);
}

void CommandList::SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    set_root_address(compute_, index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, base.ptr);
}

void CommandList::SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(compute_, index, D3D12_ROOT_PARAMETER_TYPE_CBV, address);
}

void CommandList::SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(compute_, index, D3D12_ROOT_PARAMETER_TYPE_SRV, address);
}

void CommandList::SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(compute_, index, D3D12_ROOT_PARAMETER_TYPE_UAV, address);
}

void CommandList::SetComputeRoot32BitConstant(UINT index, UINT value, UINT dest_offset)
{
    set_root_constants(compute_, index, 1, &value, dest_offset);
}

void CommandList::SetComputeRoot32BitConstants(UINT index, UINT count, const void *data, UINT dest_offset)
{
    set_root_constants(compute_, index, count, data, dest_offset);
}

// Backend resources use hazard tracking, so barriers record nothing yet.
void CommandList::ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER *)
{
}

// GPU descriptor handles are plain addresses and every allocation is resident, so root
// descriptor tables need no binding. Shaders that index the heaps directly (SM 6.6
// ResourceDescriptorHeap) find them at fixed bind points: the backend binds the current pair.
void CommandList::SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap *const *heaps)
{
    uint64_t resource_heap = 0, sampler_heap = 0;
    for (UINT i = 0; i < count && heaps; ++i) {
        auto *heap = ours<DescriptorHeap>(heaps[i]);
        if (!heap)
            continue;
        if (heap->type() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            resource_heap = heap->gpu_address();
        else if (heap->type() == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            sampler_heap = heap->gpu_address();
    }
    auto *cmd = append<mtlb_cmd_set_descriptor_heaps>(MTLB_CMD_SET_DESCRIPTOR_HEAPS);
    cmd->resource_heap = resource_heap;
    cmd->sampler_heap = sampler_heap;
}

// ---- Draws ------------------------------------------------------------------

void CommandList::DrawInstanced(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance)
{
    if (!prepare_draw())
        return;
    auto *cmd = append<mtlb_cmd_draw>(MTLB_CMD_DRAW);
    cmd->vertex_count = vertex_count;
    cmd->instance_count = instance_count;
    cmd->start_vertex = start_vertex;
    cmd->start_instance = start_instance;
}

void CommandList::DrawIndexedInstanced(UINT index_count, UINT instance_count, UINT start_index, INT base_vertex,
                                       UINT start_instance)
{
    if (!prepare_draw())
        return;
    auto *cmd = append<mtlb_cmd_draw_indexed>(MTLB_CMD_DRAW_INDEXED);
    cmd->index_count = index_count;
    cmd->instance_count = instance_count;
    cmd->start_index = start_index;
    cmd->base_vertex = base_vertex;
    cmd->start_instance = start_instance;
}

// UAV clears. The descriptor only holds Metal object ids, so the view comes from the heap's shadow table.
void CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                               ID3D12Resource *resource, const UINT values[4], UINT num_rects,
                                               const D3D12_RECT *rects)
{
    clear_uav(cpu_handle, resource, values, false, num_rects, rects);
}

void CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                ID3D12Resource *resource, const FLOAT values[4], UINT num_rects,
                                                const D3D12_RECT *rects)
{
    uint32_t bits[4];
    std::memcpy(bits, values, sizeof(bits));
    clear_uav(cpu_handle, resource, bits, true, num_rects, rects);
}

void CommandList::clear_uav(D3D12_CPU_DESCRIPTOR_HANDLE view_handle, ID3D12Resource *resource_ptr,
                            const uint32_t values[4], bool from_float, UINT num_rects, const D3D12_RECT *rects)
{
    auto *resource = ours<Resource>(resource_ptr);
    const ViewInfo *view = device()->view_info(view_handle);
    if (closed_ || !resource || !view || view->kind == ViewInfo::None) {
        D3D12M_LOG("ClearUnorderedAccessView: the view or the resource is not known to this layer");
        return;
    }
    if (view->kind == ViewInfo::Buffer) {
        if (!resource->is_buffer())
            return;
        // Structured and raw buffers are cleared with the first value in every dword; typed ones with an element.
        uint8_t pattern[16];
        uint32_t element_bytes = 4;
        uint64_t bytes_per_element = view->stride ? view->stride : 4;
        if (view->format != DXGI_FORMAT_UNKNOWN && !view->raw && !view->stride) {
            element_bytes = pack_clear_element(view->format, values, from_float, pattern);
            if (!element_bytes) {
                D3D12M_LOG("ClearUnorderedAccessView: format %d cannot be cleared", static_cast<int>(view->format));
                return;
            }
            bytes_per_element = element_bytes;
        } else {
            std::memset(pattern, 0, sizeof(pattern));
            std::memcpy(pattern, values, 4);
        }
        if (num_rects)
            D3D12M_LOG("ClearUnorderedAccessView: rectangles do not apply to buffers and are ignored");
        auto *cmd = append<mtlb_cmd_clear_buffer>(MTLB_CMD_CLEAR_BUFFER);
        cmd->buffer = resource->buffer();
        cmd->offset = view->first_element * bytes_per_element;
        cmd->size = view->num_elements * bytes_per_element;
        cmd->pattern_size = element_bytes;
        std::memcpy(cmd->pattern, pattern, sizeof(pattern));
        // A structured view of more than 4 bytes per element: fill dword by dword over the same range.
        if (view->stride && !view->raw) {
            cmd->pattern_size = 4;
            std::memset(cmd->pattern, 0, sizeof(cmd->pattern));
            std::memcpy(cmd->pattern, values, 4);
            cmd->size = (cmd->size / 4) * 4;
        }
        return;
    }

    if (resource->is_buffer())
        return;
    mtlb_format_info info;
    const DXGI_FORMAT format = view->format == DXGI_FORMAT_UNKNOWN ? resource->desc().Format : view->format;
    const bool known = get_format_info(format, &info);
    (void)known;
    // The value is written through a typed texture: float formats take floats, integer formats integers.
    uint32_t kind = from_float ? MTLB_CLEAR_FLOAT : MTLB_CLEAR_UINT;
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8_SINT:
        kind = MTLB_CLEAR_SINT;
        break;
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        kind = MTLB_CLEAR_UINT;
        break;
    default:
        if (!from_float) {
            // An integer clear of a normalized or float format: the bits go through the format's own conversion.
            float converted[4];
            for (int i = 0; i < 4; ++i)
                converted[i] = static_cast<float>(values[i]);
            D3D12M_LOG("ClearUnorderedAccessViewUint on a non-integer texture format converts the values to float");
            std::memcpy(const_cast<uint32_t *>(values), converted, sizeof(converted));
        }
        kind = MTLB_CLEAR_FLOAT;
    }
    auto emit = [&](uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        auto *cmd = append<mtlb_cmd_clear_texture_uav>(MTLB_CMD_CLEAR_TEXTURE_UAV);
        cmd->texture = resource->texture();
        cmd->view.type = view->type;
        const mtlb_format view_format = to_mtlb_format(format);
        cmd->view.format = view_format == to_mtlb_format(resource->desc().Format) ? 0 : view_format;
        cmd->view.first_mip = view->first_mip;
        cmd->view.mip_count = 1;
        cmd->view.first_slice = view->first_slice;
        cmd->view.slice_count = view->slice_count;
        cmd->kind = kind;
        std::memcpy(cmd->value, values, sizeof(cmd->value));
        cmd->x = x;
        cmd->y = y;
        cmd->width = width;
        cmd->height = height;
    };
    if (!num_rects || !rects) {
        emit(0, 0, 0, 0);
        return;
    }
    for (UINT i = 0; i < num_rects; ++i) {
        const D3D12_RECT &r = rects[i];
        if (r.right > r.left && r.bottom > r.top)
            emit(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
}

void CommandList::Dispatch(UINT x, UINT y, UINT z)
{
    if (closed_ || !has_compute_pipeline_ || !compute_.signature) {
        D3D12M_LOG("dispatch skipped: needs an open list, a compute pipeline and a compute root signature");
        return;
    }
    flush_root_args(compute_, MTLB_CMD_SET_COMPUTE_ROOT_ARGS);
    auto *cmd = append<mtlb_cmd_dispatch>(MTLB_CMD_DISPATCH);
    cmd->x = x;
    cmd->y = y;
    cmd->z = z;
}

// ---- Copies -----------------------------------------------------------------

void CommandList::CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset,
                                   UINT64 size)
{
    auto *d = ours<Resource>(dst);
    auto *s = ours<Resource>(src);
    if (!d || !s || !d->is_buffer() || !s->is_buffer()) {
        D3D12M_LOG("CopyBufferRegion needs two buffers of this layer");
        return;
    }
    auto *cmd = append<mtlb_cmd_copy_buffer>(MTLB_CMD_COPY_BUFFER);
    cmd->dst = d->buffer();
    cmd->src = s->buffer();
    cmd->dst_offset = dst_offset;
    cmd->src_offset = src_offset;
    cmd->size = size;
}

void CommandList::CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
{
    auto *d = ours<Resource>(dst);
    auto *s = ours<Resource>(src);
    if (!d || !s || d->is_buffer() != s->is_buffer()) {
        D3D12M_LOG("CopyResource needs two buffers or two textures of this layer");
        return;
    }
    if (d->is_buffer()) {
        CopyBufferRegion(dst, 0, src, 0, std::min(d->desc().Width, s->desc().Width));
        return;
    }
    // Every mip and slice, so both textures must have the same shape.
    const D3D12_RESOURCE_DESC &dd = d->desc(), &sd = s->desc();
    if (dd.Width != sd.Width || dd.Height != sd.Height || dd.DepthOrArraySize != sd.DepthOrArraySize
        || dd.MipLevels != sd.MipLevels || dd.SampleDesc.Count != sd.SampleDesc.Count) {
        D3D12M_LOG("CopyResource: the textures differ in size, mips, slices or samples");
        return;
    }
    auto *cmd = append<mtlb_cmd_copy_texture_texture>(MTLB_CMD_COPY_TEXTURE_TEXTURE);
    cmd->dst = d->texture();
    cmd->src = s->texture();
    cmd->whole = 1;
}

void CommandList::copy_texture_to_texture(const D3D12_TEXTURE_COPY_LOCATION &dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                          const D3D12_TEXTURE_COPY_LOCATION &src, const D3D12_BOX *src_box)
{
    auto *d = ours<Resource>(dst.pResource);
    auto *s = ours<Resource>(src.pResource);
    if (!d || !s || d->is_buffer() || s->is_buffer()) {
        D3D12M_LOG("CopyTextureRegion: texture copy locations need textures of this layer");
        return;
    }
    UINT src_mip, src_slice, dst_mip, dst_slice;
    decompose_subresource(s->desc(), src.SubresourceIndex, &src_mip, &src_slice);
    decompose_subresource(d->desc(), dst.SubresourceIndex, &dst_mip, &dst_slice);
    const Extent extent = subresource_extent(s->desc(), src_mip);
    D3D12_BOX box = {0, 0, 0, extent.width, extent.height, extent.depth};
    if (src_box)
        box = *src_box;
    auto *cmd = append<mtlb_cmd_copy_texture_texture>(MTLB_CMD_COPY_TEXTURE_TEXTURE);
    cmd->dst = d->texture();
    cmd->src = s->texture();
    cmd->dst_mip = dst_mip;
    cmd->dst_slice = dst_slice;
    cmd->src_mip = src_mip;
    cmd->src_slice = src_slice;
    cmd->dst_x = dst_x;
    cmd->dst_y = dst_y;
    cmd->dst_z = dst_z;
    cmd->src_x = box.left;
    cmd->src_y = box.top;
    cmd->src_z = box.front;
    cmd->width = box.right - box.left;
    cmd->height = box.bottom - box.top;
    cmd->depth = box.back - box.front;
}

// Copies between a texture subresource and a placed buffer footprint (either way), or between
// two texture subresources.
void CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                    const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box)
{
    if (!dst || !src)
        return;
    const bool to_buffer = dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
                           && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const bool to_texture = dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                            && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        copy_texture_to_texture(*dst, dst_x, dst_y, dst_z, *src, src_box);
        return;
    }
    if (!to_buffer && !to_texture) {
        D3D12M_LOG("CopyTextureRegion: unsupported combination of copy locations");
        return;
    }

    auto *buffer_resource = ours<Resource>((to_buffer ? dst : src)->pResource);
    auto *texture_resource = ours<Resource>((to_buffer ? src : dst)->pResource);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &placed = (to_buffer ? dst : src)->PlacedFootprint;
    const UINT subresource = (to_buffer ? src : dst)->SubresourceIndex;
    mtlb_format_info info;
    if (!buffer_resource || !texture_resource || !buffer_resource->is_buffer() || texture_resource->is_buffer()
        || !get_format_info(placed.Footprint.Format, &info)) {
        D3D12M_LOG("CopyTextureRegion: invalid resources or footprint format");
        return;
    }

    const D3D12_RESOURCE_DESC &td = texture_resource->desc();
    UINT mip, array_slice;
    decompose_subresource(td, subresource, &mip, &array_slice);

    // The copied region spans `box` of the source image (the whole source
    // subresource or footprint when no box is given). `texture_origin` is where
    // the region sits in the texture; `image_origin` is where it sits inside the
    // placed footprint, which describes a buffer-backed image.
    UINT box_origin[3] = {0, 0, 0}, size[3];
    if (src_box) {
        box_origin[0] = src_box->left;
        box_origin[1] = src_box->top;
        box_origin[2] = src_box->front;
        size[0] = src_box->right - src_box->left;
        size[1] = src_box->bottom - src_box->top;
        size[2] = src_box->back - src_box->front;
    } else if (to_buffer) {
        const Extent extent = subresource_extent(td, mip);
        size[0] = extent.width;
        size[1] = extent.height;
        size[2] = extent.depth;
    } else {
        // The footprint may be larger than the mip (block-rounded); stop at the texture's edge.
        const Extent extent = subresource_extent(td, mip);
        size[0] = std::min(placed.Footprint.Width, extent.width > dst_x ? extent.width - dst_x : 0);
        size[1] = std::min(placed.Footprint.Height, extent.height > dst_y ? extent.height - dst_y : 0);
        size[2] = std::min(placed.Footprint.Depth, extent.depth > dst_z ? extent.depth - dst_z : 0);
    }
    const UINT dst_origin[3] = {dst_x, dst_y, dst_z};
    const UINT *texture_origin = to_buffer ? box_origin : dst_origin;
    const UINT *image_origin = to_buffer ? dst_origin : box_origin;

    const UINT row_pitch = placed.Footprint.RowPitch;
    const UINT slice_pitch = row_pitch * ((placed.Footprint.Height + info.block_height - 1) / info.block_height);

    mtlb_texture_copy_region r{};
    r.texture = texture_resource->texture();
    r.buffer = buffer_resource->buffer();
    r.buffer_offset = placed.Offset + uint64_t(image_origin[2]) * slice_pitch
                      + uint64_t(image_origin[1] / info.block_height) * row_pitch
                      + uint64_t(image_origin[0] / info.block_width) * info.bytes_per_block;
    r.bytes_per_row = row_pitch;
    r.bytes_per_image = slice_pitch;
    r.mip_level = mip;
    r.array_slice = array_slice;
    r.x = texture_origin[0];
    r.y = texture_origin[1];
    r.z = texture_origin[2];
    r.width = size[0];
    r.height = size[1];
    r.depth = size[2];

    append<mtlb_cmd_copy_texture>(to_buffer ? MTLB_CMD_COPY_TEXTURE_TO_BUFFER : MTLB_CMD_COPY_BUFFER_TO_TEXTURE)->region = r;
}

} // namespace d3d12m
