// SPDX-License-Identifier: LGPL-2.1-or-later
// C entry points of dxgi.dll.
#include "dxgi/factory.h"

#include "common/export.h"
#include "common/trace.h"

D3D12M_EXPORT HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    D3D12M_TRACED_BEGIN
    return d3d12m::create_dxgi_factory(flags, riid, factory);
    D3D12M_TRACED_END(flags, riid, factory)
}

D3D12M_EXPORT HRESULT CreateDXGIFactory1(REFIID riid, void **factory)
{
    D3D12M_TRACED_BEGIN
    return d3d12m::create_dxgi_factory(0, riid, factory);
    D3D12M_TRACED_END(riid, factory)
}

D3D12M_EXPORT HRESULT CreateDXGIFactory(REFIID riid, void **factory)
{
    D3D12M_TRACED_BEGIN
    return d3d12m::create_dxgi_factory(0, riid, factory);
    D3D12M_TRACED_END(riid, factory)
}

D3D12M_EXPORT HRESULT DXGIGetDebugInterface1(UINT, REFIID, void **debug)
{
    D3D12M_TRACED_BEGIN
    if (debug)
        *debug = nullptr;
    return E_NOINTERFACE;
    D3D12M_TRACED_END(debug)
}

D3D12M_EXPORT HRESULT DXGIDeclareAdapterRemovalSupport()
{
    D3D12M_TRACED_BEGIN
    return S_OK;
    D3D12M_TRACED_END()
}
