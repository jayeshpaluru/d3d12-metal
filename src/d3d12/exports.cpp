// d3d12.dll entry points other than root signature serialization.
#include "common/export.h"
#include "common/log.h"
#include "d3d12/device.h"
#include "d3d12/dred.h"

using namespace d3d12m;

D3D12M_EXPORT HRESULT D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device)
{
    D3D12M_TRACED_BEGIN
    if (minimum_feature_level > max_feature_level())
        return DXGI_ERROR_UNSUPPORTED;

    ID3D12Device10 *created = nullptr;
    HRESULT hr = Device::create(adapter, &created);
    if (FAILED(hr))
        return hr;
    if (!device) {
        // A null output pointer only asks whether device creation would succeed.
        created->Release();
        return S_FALSE;
    }
    return hand_out(created, riid, device);
    D3D12M_TRACED_END(adapter, minimum_feature_level, riid, device)
}

D3D12M_EXPORT HRESULT D3D12GetDebugInterface(REFIID riid, void **debug)
{
    D3D12M_TRACED_BEGIN
    if (!debug)
        return E_INVALIDARG;
    *debug = nullptr;
    // DRED settings are accepted and ignored; the debug layer and its info queue are absent.
    if (riid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings) || riid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings1))
        return dred_settings()->QueryInterface(riid, debug);
    return E_NOINTERFACE;
    D3D12M_TRACED_END(riid, debug)
}

D3D12M_EXPORT HRESULT D3D12EnableExperimentalFeatures(UINT num_features, const IID *, void *, UINT *)
{
    D3D12M_TRACED_BEGIN
    // No experimental features exist; asking for none succeeds like on Windows.
    if (num_features != 0)
        D3D12M_LOG("D3D12EnableExperimentalFeatures: no experimental features are available");
    return num_features == 0 ? S_OK : E_NOINTERFACE;
    D3D12M_TRACED_END(num_features)
}
