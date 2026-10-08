// DXGI format helpers: mapping to bridge formats and subresource layout maths.
#pragma once

#include "common/com.h"
#include "bridge/mtlb.h"

namespace d3d12m {

// Returns the bridge format for `format`, or MTLB_FORMAT_UNKNOWN when the
// bridge does not support it (DXGI_FORMAT_UNKNOWN included).
mtlb_format to_mtlb_format(DXGI_FORMAT format);

// Block geometry and capability flags; false when the format is unsupported.
bool get_format_info(DXGI_FORMAT format, mtlb_format_info *info);

// Mip count of a resource, resolving MipLevels == 0 to a full chain.
UINT resolve_mip_levels(const D3D12_RESOURCE_DESC &desc);

// Number of array slices of a texture (1 for 3D textures).
UINT array_size(const D3D12_RESOURCE_DESC &desc);

// Implements ID3D12Device::GetCopyableFootprints for buffers and textures.
// Returns false for unsupported formats or out-of-range subresources. The
// returned size runs from `base_offset` to the end of the last subresource.
bool compute_copyable_footprints(const D3D12_RESOURCE_DESC &desc, UINT first_subresource,
                                 UINT num_subresources, UINT64 base_offset,
                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *num_rows,
                                 UINT64 *row_size_in_bytes, UINT64 *total_bytes);

} // namespace d3d12m
