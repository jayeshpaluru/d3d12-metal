// ID3DBlob implementation used for serialized root signatures and error text.
#pragma once

#include <cstddef>
#include <string>

#include "common/d3d12_uuids.h"

namespace d3d12m {

// Creates a blob holding a copy of [data, data + size). The caller owns the
// returned reference.
HRESULT create_blob(const void *data, size_t size, ID3DBlob **out);

// Creates a blob holding the characters of `text` (no terminating NUL).
HRESULT create_blob_from_string(const std::string &text, ID3DBlob **out);

} // namespace d3d12m
