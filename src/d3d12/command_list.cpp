#include "d3d12/command_list.h"

#include <algorithm>
#include <cstring>

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

UINT mip_extent(UINT size, UINT mip)
{
    return std::max(1u, size >> mip);
}

} // namespace

HRESULT CommandList::create(Device *device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator,
                            ID3D12PipelineState *initial_state, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!allocator)
        return E_INVALIDARG;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE
        && type != D3D12_COMMAND_LIST_TYPE_COPY)
        return E_INVALIDARG;
    auto *list = new CommandList(device, type);
    list->reset_state();
    if (initial_state)
        list->SetPipelineState(initial_state);
    HRESULT hr = list->QueryInterface(riid, out);
    list->Release();
    return hr;
}

CommandList::~CommandList()
{
    safe_release(root_signature_);
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
    has_pipeline_ = false;
    safe_release(root_signature_);
    root_args_.clear();
    root_args_dirty_ = false;
    // Replay state must not leak in from the previous list of the same submit.
    append<mtlb_cmd_reset_state>(MTLB_CMD_RESET_STATE);
}

// Gets the stream ready for a draw: the root arguments must be current. Returns
// false (after logging) when the draw cannot be recorded.
bool CommandList::prepare_draw()
{
    if (closed_ || !has_pipeline_ || !root_signature_) {
        D3D12M_LOG("draw skipped: needs an open list, a pipeline and a root signature");
        return false;
    }
    if (root_args_dirty_ && !root_args_.empty()) {
        auto *cmd = append<mtlb_cmd_set_graphics_root_args>(MTLB_CMD_SET_GRAPHICS_ROOT_ARGS, root_args_.size());
        cmd->data_size = static_cast<uint32_t>(root_args_.size());
        std::memcpy(cmd->data, root_args_.data(), root_args_.size());
    }
    root_args_dirty_ = false;
    return true;
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
    if (!pso)
        return;
    has_pipeline_ = true;
    append<mtlb_cmd_set_pipeline>(MTLB_CMD_SET_PIPELINE)->pipeline = static_cast<PipelineState *>(pso)->handle();
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
    if (!viewports)
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
    if (!rects)
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

void CommandList::SetGraphicsRootSignature(ID3D12RootSignature *signature)
{
    auto *rs = static_cast<RootSignature *>(signature);
    if (rs)
        rs->AddRef();
    safe_release(root_signature_);
    root_signature_ = rs;
    // Changing the root signature invalidates all root arguments.
    root_args_.assign(rs ? rs->argument_buffer_size() : 0, 0);
    root_args_dirty_ = true;
}

const RootSignature::Slot *CommandList::find_slot(UINT index, D3D12_ROOT_PARAMETER_TYPE type)
{
    if (!root_signature_ || index >= root_signature_->slots().size()
        || root_signature_->slots()[index].type != type) {
        D3D12M_LOG("root parameter %u is not set up for this kind of argument", index);
        return nullptr;
    }
    return &root_signature_->slots()[index];
}

void CommandList::set_root_address(UINT index, D3D12_ROOT_PARAMETER_TYPE type, uint64_t address)
{
    if (const RootSignature::Slot *slot = find_slot(index, type)) {
        std::memcpy(root_args_.data() + slot->offset, &address, sizeof(address));
        root_args_dirty_ = true;
    }
}

void CommandList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    set_root_address(index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, base.ptr);
}

void CommandList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(index, D3D12_ROOT_PARAMETER_TYPE_CBV, address);
}

void CommandList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(index, D3D12_ROOT_PARAMETER_TYPE_SRV, address);
}

void CommandList::SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    set_root_address(index, D3D12_ROOT_PARAMETER_TYPE_UAV, address);
}

void CommandList::SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT dest_offset)
{
    SetGraphicsRoot32BitConstants(index, 1, &value, dest_offset);
}

void CommandList::SetGraphicsRoot32BitConstants(UINT index, UINT count, const void *data, UINT dest_offset)
{
    const RootSignature::Slot *slot = find_slot(index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    if (!slot || !data)
        return;
    if ((uint64_t(dest_offset) + count) * 4 > slot->size) {
        D3D12M_LOG("root constants for parameter %u exceed its %u bytes", index, slot->size);
        return;
    }
    std::memcpy(root_args_.data() + slot->offset + dest_offset * 4, data, count * 4);
    root_args_dirty_ = true;
}

// Backend resources use hazard tracking, so barriers record nothing yet.
void CommandList::ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER *)
{
}

// GPU descriptor handles are plain addresses and every allocation is resident,
// so there is nothing to bind.
void CommandList::SetDescriptorHeaps(UINT, ID3D12DescriptorHeap *const *)
{
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

// ---- Copies -----------------------------------------------------------------

void CommandList::CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset,
                                   UINT64 size)
{
    auto *d = static_cast<Resource *>(dst);
    auto *s = static_cast<Resource *>(src);
    if (!d || !s || !d->is_buffer() || !s->is_buffer()) {
        D3D12M_LOG("CopyBufferRegion needs two buffers");
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
    auto *d = static_cast<Resource *>(dst);
    auto *s = static_cast<Resource *>(src);
    if (!d || !s || !d->is_buffer() || !s->is_buffer()) {
        D3D12M_STUB_LOG();  // texture copies are not implemented
        return;
    }
    CopyBufferRegion(dst, 0, src, 0, std::min(d->desc().Width, s->desc().Width));
}

// Supports the two directions the backend can express: texture subresource to
// a placed buffer footprint and back.
void CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                    const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box)
{
    if (!dst || !src)
        return;
    const bool to_buffer = dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
                           && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const bool to_texture = dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                            && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    if (!to_buffer && !to_texture) {
        D3D12M_STUB_LOG();  // texture-to-texture copies are not implemented
        return;
    }

    auto *buffer_resource = static_cast<Resource *>((to_buffer ? dst : src)->pResource);
    auto *texture_resource = static_cast<Resource *>((to_buffer ? src : dst)->pResource);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &placed = (to_buffer ? dst : src)->PlacedFootprint;
    const UINT subresource = (to_buffer ? src : dst)->SubresourceIndex;
    mtlb_format_info info;
    if (!buffer_resource || !texture_resource || !buffer_resource->is_buffer() || texture_resource->is_buffer()
        || !get_format_info(placed.Footprint.Format, &info)) {
        D3D12M_LOG("CopyTextureRegion: invalid resources or footprint format");
        return;
    }

    const D3D12_RESOURCE_DESC &td = texture_resource->desc();
    const UINT mips = td.MipLevels;
    const UINT mip = subresource % mips;

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
        size[0] = mip_extent(static_cast<UINT>(td.Width), mip);
        size[1] = mip_extent(td.Height, mip);
        size[2] = td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(td.DepthOrArraySize, mip) : 1;
    } else {
        size[0] = placed.Footprint.Width;
        size[1] = placed.Footprint.Height;
        size[2] = placed.Footprint.Depth;
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
    r.array_slice = subresource / mips;
    r.x = texture_origin[0];
    r.y = texture_origin[1];
    r.z = texture_origin[2];
    r.width = size[0];
    r.height = size[1];
    r.depth = size[2];

    if (to_buffer)
        append<mtlb_cmd_copy_texture_to_buffer>(MTLB_CMD_COPY_TEXTURE_TO_BUFFER)->region = r;
    else
        append<mtlb_cmd_copy_buffer_to_texture>(MTLB_CMD_COPY_BUFFER_TO_TEXTURE)->region = r;
}

} // namespace d3d12m
