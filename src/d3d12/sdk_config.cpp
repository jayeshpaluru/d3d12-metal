// SPDX-License-Identifier: LGPL-2.1-or-later
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

// The class IDs, which the MinGW headers declare without defining (or lack).
const GUID kClsidSdkConfiguration = {0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
const GUID kClsidRemovedExtendedData = {0x4a75bbc4, 0x9ff4, 0x4ad8, {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};
const GUID kClsidDeviceFactory = {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};

class DeviceFactory final : public ID3D12DeviceFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12DeviceFactory>(this, riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }
    HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override { D3D12M_TRACED_BEGIN return S_OK; D3D12M_TRACED_END() }
    HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) override
    {
        D3D12M_TRACED_BEGIN
        flags_ = flags;
        return S_OK;
        D3D12M_TRACED_END(flags)
    }
    D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() override { D3D12M_TRACE(); return flags_; }
    HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid, REFIID iid, void **out) override;
    HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT count, const IID *, void *, UINT *) override
    {
        D3D12M_TRACED_BEGIN
        return count == 0 ? S_OK : E_NOINTERFACE;
        D3D12M_TRACED_END(count)
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL feature_level, REFIID riid, void **device) override
    {
        D3D12M_TRACED_BEGIN
        return D3D12CreateDevice(adapter, feature_level, riid, device);
        D3D12M_TRACED_END(adapter, feature_level, riid, device)
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
        D3D12M_TRACED_BEGIN
        D3D12M_LOG("SetSDKVersion(%u, \"%s\") ignored: the layer is the runtime", version, path ? path : "");
        return S_OK;
        D3D12M_TRACED_END(version, path)
    }
    HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT, const char *, REFIID riid, void **out) override
    {
        D3D12M_TRACED_BEGIN
        return g_factory.QueryInterface(riid, out);
        D3D12M_TRACED_END(riid, out)
    }
    void STDMETHODCALLTYPE FreeUnusedSDKs() override {}
};

SdkConfiguration g_configuration;

} // namespace

HRESULT DeviceFactory::GetConfigurationInterface(REFCLSID clsid, REFIID iid, void **out)
{
    D3D12M_TRACED_BEGIN
    return D3D12GetInterface(clsid, iid, out);
    D3D12M_TRACED_END(clsid, iid, out)
}

D3D12M_EXPORT HRESULT D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    D3D12M_TRACED_BEGIN
    if (!object)
        return E_INVALIDARG;
    *object = nullptr;
    if (clsid == kClsidSdkConfiguration)
        return g_configuration.QueryInterface(iid, object);
    if (clsid == kClsidDeviceFactory)
        return g_factory.QueryInterface(iid, object);
    if (clsid == kClsidRemovedExtendedData)
        return dred_settings()->QueryInterface(iid, object);
    // The debug layer and the tools are not provided.
    D3D12M_LOG("D3D12GetInterface: no object for this class");
    return E_NOINTERFACE;
    D3D12M_TRACED_END(clsid, iid, object)
}
