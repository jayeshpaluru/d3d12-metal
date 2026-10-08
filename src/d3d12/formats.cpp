#include "d3d12/formats.h"

#include <algorithm>

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
    UINT64 end = base_offset;

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (first_subresource != 0 || num_subresources > 1)
            return false;
        if (num_subresources == 1) {
            if (layouts) {
                layouts[0].Offset = base_offset;
                layouts[0].Footprint = {DXGI_FORMAT_UNKNOWN, static_cast<UINT>(desc.Width), 1, 1,
                                        static_cast<UINT>(desc.Width)};
            }
            if (num_rows)
                num_rows[0] = 1;
            if (row_size_in_bytes)
                row_size_in_bytes[0] = desc.Width;
            end += desc.Width;
        }
        if (total_bytes)
            *total_bytes = end - base_offset;
        return true;
    }

    mtlb_format_info info;
    if (!get_format_info(desc.Format, &info))
        return false;
    const UINT mips = resolve_mip_levels(desc);
    if (uint64_t(first_subresource) + num_subresources > uint64_t(mips) * array_size(desc))
        return false;

    for (UINT i = 0; i < num_subresources; ++i) {
        const UINT mip = (first_subresource + i) % mips;
        const UINT width = mip_extent(static_cast<UINT>(desc.Width), mip);
        const UINT height = mip_extent(desc.Height, mip);
        const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? mip_extent(desc.DepthOrArraySize, mip) : 1;
        const UINT rows = (height + info.block_height - 1) / info.block_height;
        const UINT64 row_size = UINT64((width + info.block_width - 1) / info.block_width) * info.bytes_per_block;
        const UINT64 row_pitch = align_up(row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        const UINT64 offset = align_up(end, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (layouts) {
            layouts[i].Offset = offset;
            layouts[i].Footprint = {desc.Format, width, height, depth, static_cast<UINT>(row_pitch)};
        }
        if (num_rows)
            num_rows[i] = rows;
        if (row_size_in_bytes)
            row_size_in_bytes[i] = row_size;
        // The last row of the last slice is not padded to the pitch.
        end = offset + row_pitch * rows * (depth - 1) + row_pitch * (rows - 1) + row_size;
    }
    if (total_bytes)
        *total_bytes = end - base_offset;
    return true;
}

} // namespace d3d12m
