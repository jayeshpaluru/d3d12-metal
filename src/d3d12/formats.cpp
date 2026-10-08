#include "d3d12/formats.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace d3d12m {

namespace {

UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

UINT mip_extent(UINT size, UINT mip)
{
    return std::max(1u, size >> mip);
}

} // namespace

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
    const UINT mips = is_buffer ? 1 : resolve_mip_levels(desc);
    const UINT64 subresource_count = is_buffer ? 1 : uint64_t(mips) * array_size(desc);
    if (uint64_t(first_subresource) + num_subresources > subresource_count)
        return false;

    // Offsets are aligned relative to base_offset, as in vkd3d-proton.
    UINT64 offset = 0, total = 0;
    for (UINT i = 0; i < num_subresources; ++i) {
        const UINT mip = (first_subresource + i) % mips;
        // Block-compressed extents are rounded up to whole blocks.
        const UINT width = static_cast<UINT>(align_up(mip_extent(static_cast<UINT>(desc.Width), mip), info.block_width));
        const UINT height = static_cast<UINT>(align_up(is_buffer ? 1 : mip_extent(desc.Height, mip), info.block_height));
        const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? mip_extent(desc.DepthOrArraySize, mip) : 1;
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
