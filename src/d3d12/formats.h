// DXGI format helpers: mapping to bridge formats and subresource layout maths.
#pragma once

#include <algorithm>

#include "common/com.h"
#include "bridge/mtlb.h"

namespace d3d12m {

inline UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Size of mip level `mip` along an axis of `size` texels.
inline UINT mip_extent(UINT size, UINT mip)
{
    return std::max(1u, size >> mip);
}

// Returns the bridge format for `format`, or MTLB_FORMAT_UNKNOWN when the
// bridge does not support it (DXGI_FORMAT_UNKNOWN included).
mtlb_format to_mtlb_format(DXGI_FORMAT format);

// Block geometry and capability flags; false when the format is unsupported.
bool get_format_info(DXGI_FORMAT format, mtlb_format_info *info);

// Mip count of a resource, resolving MipLevels == 0 to a full chain.
UINT resolve_mip_levels(const D3D12_RESOURCE_DESC &desc);

// Number of array slices of a texture (1 for 3D textures).
UINT array_size(const D3D12_RESOURCE_DESC &desc);

// Planes of a format: 2 for depth-stencil formats (depth, then stencil), else 1.
UINT plane_count(DXGI_FORMAT format);

// For a plane of a depth-stencil format: the format and size of a texel of that plane in a buffer
// footprint. Depth is always read and written as 4 bytes (a 32-bit float: D24 is stored as D32 here),
// stencil as one byte.
DXGI_FORMAT plane_format(DXGI_FORMAT format, UINT plane);
UINT plane_bytes_per_texel(DXGI_FORMAT format, UINT plane);

// Number of subresources: mips times array slices times planes, or 1 for a buffer.
UINT subresource_count(const D3D12_RESOURCE_DESC &desc);

// Texel size of mip level `mip` of a texture; a buffer is {Width, 1, 1}.
struct Extent {
    UINT width, height, depth;
};
Extent subresource_extent(const D3D12_RESOURCE_DESC &desc, UINT mip);

// Splits a subresource index into its mip level, array slice and plane.
void decompose_subresource(const D3D12_RESOURCE_DESC &desc, UINT subresource, UINT *mip, UINT *array_slice,
                           UINT *plane = nullptr);

// Implements ID3D12Device::GetCopyableFootprints for buffers and textures.
// Returns false for unsupported formats or out-of-range subresources. The
// returned size runs from `base_offset` to the end of the last subresource.
bool compute_copyable_footprints(const D3D12_RESOURCE_DESC &desc, UINT first_subresource,
                                 UINT num_subresources, UINT64 base_offset,
                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows,
                                 UINT64 *row_size_in_bytes, UINT64 *total_bytes);

} // namespace d3d12m
