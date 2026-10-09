// SPDX-License-Identifier: LGPL-2.1-or-later
// DXGI swap chains (IDXGISwapChain .. IDXGISwapChain4) for D3D12 command queues.
#pragma once

#include "dxgi/dxgi_interfaces.h"

namespace d3d12m {

// Creates a swap chain for `window` presenting through `queue` (the
// ID3D12CommandQueue passed to CreateSwapChain*). Zero width or height take the
// window's client size. Returns the creation reference.
HRESULT create_swap_chain(IDXGIFactory *factory, IUnknown *queue, HWND window, const DXGI_SWAP_CHAIN_DESC1 &desc,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGISwapChain1 **out);

// The legacy-description variant (IDXGIFactory::CreateSwapChain).
HRESULT create_swap_chain(IDXGIFactory *factory, IUnknown *queue, const DXGI_SWAP_CHAIN_DESC &desc,
                          IDXGISwapChain **out);

} // namespace d3d12m
