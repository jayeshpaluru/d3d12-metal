// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dxgi/factory.h"

#include <vector>

#include "common/com.h"
#include "common/log.h"
#include "dxgi/adapter.h"
#include "dxgi/swapchain.h"
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
        D3D12M_TRACED_BEGIN
        return QueryInterface(riid, parent);
        D3D12M_TRACED_END(riid, parent)
    }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter **adapter) override
    {
        D3D12M_TRACED_BEGIN
        return enum_adapter(index, adapter);
        D3D12M_TRACED_END(index, adapter)
    }

    // Recorded only: the layer never handles Alt+Enter or other window messages itself.
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT) override
    {
        D3D12M_TRACED_BEGIN
        associated_window_ = window;
        return S_OK;
        D3D12M_TRACED_END(window)
    }

    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *window) override
    {
        D3D12M_TRACED_BEGIN
        if (!window)
            return E_INVALIDARG;
        *window = associated_window_;
        return S_OK;
        D3D12M_TRACED_END(window)
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *queue, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc || !swap_chain)
            return DXGI_ERROR_INVALID_CALL;
        return create_swap_chain(this, queue, *desc, swap_chain);
        D3D12M_TRACED_END(queue, desc, swap_chain)
    }

    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter **adapter) override
    {
        D3D12M_TRACED_BEGIN
        if (adapter)
            *adapter = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
        D3D12M_TRACED_END(adapter)
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1 **adapter) override
    {
        D3D12M_TRACED_BEGIN
        return enum_adapter(index, adapter);
        D3D12M_TRACED_END(index, adapter)
    }

    BOOL STDMETHODCALLTYPE IsCurrent() override { D3D12M_TRACE(); return TRUE; }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { D3D12M_TRACE(); return FALSE; }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *queue, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,
                                                     IDXGIOutput *, IDXGISwapChain1 **swap_chain) override
    {
        D3D12M_TRACED_BEGIN
        if (!desc || !swap_chain)
            return DXGI_ERROR_INVALID_CALL;
        return create_swap_chain(this, queue, window, *desc, fullscreen, swap_chain);
        D3D12M_TRACED_END(queue, window, desc, fullscreen, swap_chain)
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
    UINT STDMETHODCALLTYPE GetCreationFlags() override { D3D12M_TRACE(); return flags_; }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void **adapter) override
    {
        D3D12M_TRACED_BEGIN
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
        D3D12M_TRACED_END(luid, riid, adapter)
    }

    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID, void **adapter) override
    {
        D3D12M_TRACED_BEGIN
        if (adapter)
            *adapter = nullptr;
        return DXGI_ERROR_UNSUPPORTED;
        D3D12M_TRACED_END(adapter)
    }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void *data, UINT size) override
    {
        D3D12M_TRACED_BEGIN
        if (feature != DXGI_FEATURE_PRESENT_ALLOW_TEARING)
            return E_INVALIDARG;
        if (!data || size != sizeof(BOOL))
            return E_INVALIDARG;
        *static_cast<BOOL *>(data) = TRUE;
        return S_OK;
        D3D12M_TRACED_END(feature, data, size)
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
    HWND associated_window_ = {};
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
