#include "d3d12/formats.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace d3d12m {

mtlb_format to_mtlb_format(DXGI_FORMAT format)
{
    mtlb_format_info info;
    auto candidate = static_cast<mtlb_format>(format);
    return mtlb_format_get_info(candidate, &info) == MTLB_OK ? candidate : MTLB_FORMAT_UNKNOWN;
}

bool get_format_info(DXGI_FORMAT format, mtlb_format_info *info)
{
    return mtlb_format_get_info(static_cast<mtlb_format>(format), info) == MTLB_OK;
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

UINT subresource_count(const D3D12_RESOURCE_DESC &desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : resolve_mip_levels(desc) * array_size(desc);
}

Extent subresource_extent(const D3D12_RESOURCE_DESC &desc, UINT mip)
{
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return {static_cast<UINT>(desc.Width), 1, 1};
    return {mip_extent(static_cast<UINT>(desc.Width), mip), mip_extent(desc.Height, mip),
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(desc.DepthOrArraySize, mip) : 1};
}

void decompose_subresource(const D3D12_RESOURCE_DESC &desc, UINT subresource, UINT *mip, UINT *array_slice)
{
    // Single-plane formats only.
    const UINT mip_levels = resolve_mip_levels(desc);
    *mip = subresource % mip_levels;
    *array_slice = (subresource / mip_levels) % array_size(desc);
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
        UINT mip, array_slice;
        decompose_subresource(desc, first_subresource + i, &mip, &array_slice);
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
            layouts[i].Footprint = {is_buffer ? DXGI_FORMAT_UNKNOWN : desc.Format, width, height, depth,
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
