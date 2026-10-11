// SPDX-License-Identifier: LGPL-2.1-or-later
// The DXGI factory (IDXGIFactory .. IDXGIFactory5).
#pragma once

#include "dxgi/dxgi_interfaces.h"

namespace d3d12m {

// Creates a factory and returns the interface `riid` of it, like
// CreateDXGIFactory2 does. `flags` are the CreateDXGIFactory2 flags.
HRESULT create_dxgi_factory(UINT flags, REFIID riid, void **out);

} // namespace d3d12m
