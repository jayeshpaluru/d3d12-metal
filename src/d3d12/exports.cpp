// d3d12.dll entry points other than root signature serialization.
#include "common/export.h"
#include "common/log.h"
#include "d3d12/device.h"

using namespace d3d12m;

D3D12M_EXPORT HRESULT D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device)
{
    if (minimum_feature_level > D3D_FEATURE_LEVEL_12_0)
        return DXGI_ERROR_UNSUPPORTED;

    ID3D12Device2 *created = nullptr;
    HRESULT hr = Device::create(adapter, &created);
    if (FAILED(hr))
        return hr;
    if (!device) {
        // A null output pointer only asks whether device creation would succeed.
        created->Release();
        return S_FALSE;
    }
    return hand_out(created, riid, device);
}

D3D12M_EXPORT HRESULT D3D12GetDebugInterface(REFIID, void **debug)
{
    if (debug)
        *debug = nullptr;
    return E_NOINTERFACE;
}
