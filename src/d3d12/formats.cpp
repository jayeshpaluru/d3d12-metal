// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12/formats.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <climits>
#include <cstring>

namespace d3d12m {

namespace {

// The format table lives in the backend (a bridge call, a unix call under Wine); every descriptor write
// asks about formats, so the answers are cached here, one slot per DXGI_FORMAT value.
constexpr size_t kCachedFormats = 256;
enum : uint8_t { kUnknown = 0, kSupported, kUnsupported };
std::atomic<uint8_t> g_format_state[kCachedFormats];
mtlb_format_info g_format_info[kCachedFormats];
std::mutex g_format_mutex;

bool lookup_format(DXGI_FORMAT format, mtlb_format_info *info)
{
    const auto index = static_cast<size_t>(format);
    if (index >= kCachedFormats)
        return mtlb_format_get_info(static_cast<mtlb_format>(format), info) == MTLB_OK;
    uint8_t state = g_format_state[index].load(std::memory_order_acquire);
    if (state == kUnknown) {
        std::lock_guard<std::mutex> lock(g_format_mutex);
        state = g_format_state[index].load(std::memory_order_relaxed);
        if (state == kUnknown) {
            state = mtlb_format_get_info(static_cast<mtlb_format>(format), &g_format_info[index]) == MTLB_OK ? kSupported
                                                                                                          : kUnsupported;
            g_format_state[index].store(state, std::memory_order_release);
        }
    }
    if (state != kSupported)
        return false;
    *info = g_format_info[index];
    return true;
}

} // namespace

mtlb_format to_mtlb_format(DXGI_FORMAT format)
{
    mtlb_format_info info;
    return lookup_format(format, &info) ? static_cast<mtlb_format>(format) : MTLB_FORMAT_UNKNOWN;
}

bool get_format_info(DXGI_FORMAT format, mtlb_format_info *info)
{
    return lookup_format(format, info);
}

UINT resolve_mip_levels(const D3D12_RESOURCE_DESC &desc)
{
    if (desc.MipLevels != 0)
        return desc.MipLevels;
    UINT largest = std::max(desc.Width > UINT_MAX ? UINT_MAX : static_cast<UINT>(desc.Width), desc.Height);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        largest = std::max<UINT>(largest, desc.DepthOrArraySize);
    UINT levels = 1;
    while (largest >>= 1)
        ++levels;
    return levels;
}

UINT array_size(const D3D12_RESOURCE_DESC &desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
}

UINT plane_count(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return 2;
    default:
        return 1;
    }
}

DXGI_FORMAT plane_format(DXGI_FORMAT format, UINT plane)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return plane == 0 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return plane == 0 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
        return format;
    }
}

UINT plane_bytes_per_texel(DXGI_FORMAT format, UINT plane)
{
    if (plane_count(format) == 1) {
        mtlb_format_info info;
        return get_format_info(format, &info) ? info.bytes_per_block : 0;
    }
    return plane == 0 ? 4 : 1;
}

UINT subresource_count(const D3D12_RESOURCE_DESC &desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
               ? 1
               : resolve_mip_levels(desc) * array_size(desc) * plane_count(desc.Format);
}

Extent subresource_extent(const D3D12_RESOURCE_DESC &desc, UINT mip)
{
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return {static_cast<UINT>(desc.Width), 1, 1};
    return {mip_extent(static_cast<UINT>(desc.Width), mip), mip_extent(desc.Height, mip),
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(desc.DepthOrArraySize, mip) : 1};
}

void decompose_subresource(const D3D12_RESOURCE_DESC &desc, UINT subresource, UINT *mip, UINT *array_slice,
                           UINT *plane)
{
    const UINT mip_levels = resolve_mip_levels(desc);
    const UINT slices = array_size(desc);
    *mip = subresource % mip_levels;
    *array_slice = (subresource / mip_levels) % slices;
    if (plane)
        *plane = subresource / (mip_levels * slices);
}

bool compute_copyable_footprints(const D3D12_RESOURCE_DESC &desc, UINT first_subresource,
                                 UINT num_subresources, UINT64 base_offset,
                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows,
                                 UINT64 *row_size_in_bytes, UINT64 *total_bytes)
{
    // Failure leaves every output all-ones, like the native runtime.
    if (layouts)
        std::memset(layouts, 0xff, sizeof(*layouts) * num_subresources);
    if (num_rows)
        std::memset(num_rows, 0xff, sizeof(*num_rows) * num_subresources);
    if (row_size_in_bytes)
        std::memset(row_size_in_bytes, 0xff, sizeof(*row_size_in_bytes) * num_subresources);
    if (total_bytes)
        *total_bytes = UINT64_MAX;

    // A buffer is one subresource of byte-sized 1x1 blocks.
    const bool is_buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    mtlb_format_info info = {1, 1, 1, 0};
    if (!is_buffer && !get_format_info(desc.Format, &info))
        return false;
    if (uint64_t(first_subresource) + num_subresources > subresource_count(desc))
        return false;

    // Offsets are aligned relative to base_offset, as in vkd3d-proton.
    UINT64 offset = 0, total = 0;
    for (UINT i = 0; i < num_subresources; ++i) {
        UINT mip, array_slice, plane;
        decompose_subresource(desc, first_subresource + i, &mip, &array_slice, &plane);
        const bool planar = !is_buffer && plane_count(desc.Format) > 1;
        if (planar)
            info = {1, 1, plane_bytes_per_texel(desc.Format, plane), 0};
        const DXGI_FORMAT footprint_format = planar ? plane_format(desc.Format, plane) : desc.Format;
        const Extent extent = subresource_extent(desc, mip);
        // Block-compressed extents are rounded up to whole blocks.
        const UINT width = static_cast<UINT>(align_up(extent.width, info.block_width));
        const UINT height = static_cast<UINT>(align_up(extent.height, info.block_height));
        const UINT depth = extent.depth;
        const UINT rows = height / info.block_height;
        const UINT64 row_size = UINT64(width / info.block_width) * info.bytes_per_block;
        const UINT64 row_pitch = align_up(row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        if (layouts) {
            layouts[i].Offset = base_offset + offset;
            layouts[i].Footprint = {is_buffer ? DXGI_FORMAT_UNKNOWN : footprint_format, width, height, depth,
                                    static_cast<UINT>(row_pitch)};
        }
        if (num_rows)
            num_rows[i] = rows;
        if (row_size_in_bytes)
            row_size_in_bytes[i] = row_size;

        // The last row of the last slice is not padded to the pitch.
        const UINT64 slice = row_pitch * (rows - 1) + row_size;
        const UINT64 size = align_up(slice, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * (depth - 1) + slice;
        total = offset + size;
        offset = align_up(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }
    if (total_bytes)
        *total_bytes = total;
    return true;
}

} // namespace d3d12m
