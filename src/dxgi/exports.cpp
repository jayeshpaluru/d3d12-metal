// C entry points of dxgi.dll.
#include "dxgi/factory.h"

#include "common/export.h"

D3D12M_EXPORT HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    return d3d12m::create_dxgi_factory(flags, riid, factory);
}

D3D12M_EXPORT HRESULT CreateDXGIFactory1(REFIID riid, void **factory)
{
    return d3d12m::create_dxgi_factory(0, riid, factory);
}

D3D12M_EXPORT HRESULT CreateDXGIFactory(REFIID riid, void **factory)
{
    return d3d12m::create_dxgi_factory(0, riid, factory);
}
