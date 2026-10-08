// The single DXGI adapter, backed by the system's default Metal device.
#pragma once

#include "dxgi/dxgi_interfaces.h"

namespace d3d12m {

// Creates an adapter whose GetParent() returns `parent`. The adapter keeps a
// reference on `parent`. Fails with DXGI_ERROR_UNSUPPORTED if there is no
// Metal device.
HRESULT create_adapter(IDXGIFactory *parent, IDXGIAdapter3 **out);

} // namespace d3d12m
