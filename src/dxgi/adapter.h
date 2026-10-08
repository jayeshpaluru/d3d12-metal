// DXGI adapters, one per Metal device.
#pragma once

#include "bridge/mtlb.h"
#include "dxgi/dxgi_interfaces.h"

namespace d3d12m {

// Creates an adapter for the described device whose GetParent() returns
// `parent`. The adapter keeps a reference on `parent`.
IDXGIAdapter3 *create_adapter(IDXGIFactory *parent, const mtlb_device_caps &caps);

} // namespace d3d12m
