// SPDX-License-Identifier: LGPL-2.1-or-later
// DXBC (Shader Model 4/5) shaders: detection and conversion to DXIL (see dxbc.mm).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mtlb {

// True for a DXBC container without a DXIL part (what FXC and D3DCompile produce for SM 5.1 and below).
bool is_dxbc_only(const void *data, uint64_t size);

// Converts a DXBC container to a DXIL container. On failure returns false with `error` set.
bool dxbc_to_dxil(const void *dxbc, uint64_t size, std::vector<uint8_t> &dxil, std::string &error);

} // namespace mtlb
