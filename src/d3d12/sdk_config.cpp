// D3D12GetInterface and the Agility SDK configuration objects. The layer is the only D3D12 runtime there is,
// so an application's choice of SDK version and path is accepted and ignored, and the device factory creates
// devices like D3D12CreateDevice does.
#include "common/export.h"
#include "common/log.h"
#include "d3d12/device.h"
#include "d3d12/dred.h"

using namespace d3d12m;

extern "C" HRESULT D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device);

namespace {

class DeviceFactory final : public ID3D12DeviceFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12DeviceFactory>(this, riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) override
    {
        flags_ = flags;
        return S_OK;
    }
    D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() override { return flags_; }
    HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid, REFIID iid, void **out) override;
    HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT count, const IID *, void *, UINT *) override
    {
        return count == 0 ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL feature_level, REFIID riid, void **device) override
    {
        return D3D12CreateDevice(adapter, feature_level, riid, device);
    }

private:
    D3D12_DEVICE_FACTORY_FLAGS flags_ = D3D12_DEVICE_FACTORY_FLAG_NONE;
};

DeviceFactory g_factory;

class SdkConfiguration final : public ID3D12SDKConfiguration1 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12SDKConfiguration, ID3D12SDKConfiguration1>(this, riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT version, const char *path) override
    {
        D3D12M_LOG("SetSDKVersion(%u, \"%s\") ignored: the layer is the runtime", version, path ? path : "");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT, const char *, REFIID riid, void **out) override
    {
        return g_factory.QueryInterface(riid, out);
    }
    void STDMETHODCALLTYPE FreeUnusedSDKs() override {}
};

SdkConfiguration g_configuration;

} // namespace

HRESULT DeviceFactory::GetConfigurationInterface(REFCLSID clsid, REFIID iid, void **out)
{
    return D3D12GetInterface(clsid, iid, out);
}

D3D12M_EXPORT HRESULT D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    if (!object)
        return E_INVALIDARG;
    *object = nullptr;
    if (clsid == CLSID_D3D12SDKConfiguration)
        return g_configuration.QueryInterface(iid, object);
    if (clsid == CLSID_D3D12DeviceFactory)
        return g_factory.QueryInterface(iid, object);
    if (clsid == CLSID_D3D12DeviceRemovedExtendedData)
        return dred_settings()->QueryInterface(iid, object);
    // The debug layer and the tools are not provided.
    D3D12M_LOG("D3D12GetInterface: no object for this class");
    return E_NOINTERFACE;
}
