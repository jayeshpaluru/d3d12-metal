#include "dxgi/factory.h"

#include <vector>

#include "common/com.h"
#include "common/log.h"
#include "dxgi/adapter.h"
#include "common/private_data.h"

namespace d3d12m {

namespace {

class Factory final : public WithPrivateData<RefCounted<IDXGIFactory5>> {
public:
    // Describes the adapters once; the default Metal device comes first.
    explicit Factory(UINT flags) : flags_(flags)
    {
        mtlb_device_caps caps;
        if (mtlb_query_caps(0, &caps) != MTLB_OK)
            return;
        devices_.push_back(caps);
        for (uint32_t i = 0; mtlb_enum_devices(i, &caps) == MTLB_OK; ++i) {
            if (caps.registry_id != devices_[0].registry_id)
                devices_.push_back(caps);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, IDXGIObject, IDXGIFactory, IDXGIFactory1, IDXGIFactory2,
                                IDXGIFactory3, IDXGIFactory4, IDXGIFactory5>(this, riid, out);
    }

    // A factory has no parent object; like the Windows one, answer with itself.
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override
    {
        return QueryInterface(riid, parent);
    }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter **adapter) override
    {
        return enum_adapter(index, adapter);
    }

    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *window) override
    {
        if (!window)
            return E_INVALIDARG;
        *window = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter **adapter) override
    {
        if (adapter)
            *adapter = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1 **adapter) override
    {
        return enum_adapter(index, adapter);
    }

    BOOL STDMETHODCALLTYPE IsCurrent() override { return TRUE; }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                                                     IDXGIOutput *, IDXGISwapChain1 **) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *, IUnknown *,
                                                           const DXGI_SWAP_CHAIN_DESC1 *,
                                                           IDXGIOutput *, IDXGISwapChain1 **) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE, LUID *) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override { D3D12M_STUB_LOG(); }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override { D3D12M_STUB_LOG(); }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                                            IDXGIOutput *, IDXGISwapChain1 **) override
    {
        D3D12M_STUB_HR();
    }

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void **adapter) override
    {
        IDXGIAdapter1 *found = nullptr;
        HRESULT hr = enum_adapter(0, &found);
        if (FAILED(hr))
            return hr;

        DXGI_ADAPTER_DESC1 desc;
        found->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart != luid.LowPart || desc.AdapterLuid.HighPart != luid.HighPart) {
            found->Release();
            return DXGI_ERROR_NOT_FOUND;
        }
        return hand_out(found, riid, adapter);
    }

    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID, void **adapter) override
    {
        if (adapter)
            *adapter = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void *data, UINT size) override
    {
        if (feature != DXGI_FEATURE_PRESENT_ALLOW_TEARING)
            return E_INVALIDARG;
        if (!data || size != sizeof(BOOL))
            return E_INVALIDARG;
        *static_cast<BOOL *>(data) = TRUE;
        return S_OK;
    }

private:
    template <typename AdapterInterface>
    HRESULT enum_adapter(UINT index, AdapterInterface **out)
    {
        if (!out)
            return E_INVALIDARG;
        *out = nullptr;
        if (index >= devices_.size())
            return DXGI_ERROR_NOT_FOUND;
        return hand_out(create_adapter(this, devices_[index]), __uuidof(AdapterInterface),
                        reinterpret_cast<void **>(out));
    }

    UINT flags_;
    std::vector<mtlb_device_caps> devices_;
};

} // namespace

HRESULT create_dxgi_factory(UINT flags, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;

    Factory *factory = new Factory(flags);
    return hand_out(factory, riid, out);
}

} // namespace d3d12m
