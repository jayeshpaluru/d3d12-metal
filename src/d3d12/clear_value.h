// Clear values of UAV clears converted to the bytes of one element of a format.
#pragma once

#include <cstdint>

#include "common/com.h"

namespace d3d12m {

// Packs one element of `format`. `values` hold four unsigned integers (ClearUnorderedAccessViewUint) or,
// when `from_float`, the bits of four floats (ClearUnorderedAccessViewFloat). Integer values keep the bits
// the component has; float values are converted by the format (clamped and scaled for normalized
// formats, half precision for 16-bit floats). Returns the element size in bytes, or 0 when the format has
// no packing here.
uint32_t pack_clear_element(DXGI_FORMAT format, const uint32_t values[4], bool from_float, uint8_t out[16]);

} // namespace d3d12m
