// The one DXGI output (display) of every adapter.
#pragma once

#include "dxgi/dxgi_interfaces.h"

namespace d3d12m {

// Creates the output whose GetParent() returns `parent`; keeps a reference on it.
IDXGIOutput6 *create_output(IDXGIAdapter *parent);

} // namespace d3d12m
