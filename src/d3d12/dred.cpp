#include "d3d12/dred.h"

namespace d3d12m {

namespace {

constexpr HRESULT kNotAvailable = static_cast<HRESULT>(0x887A0022);  // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE

class DredSettings final : public ID3D12DeviceRemovedExtendedDataSettings1 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12DeviceRemovedExtendedDataSettings,
                                ID3D12DeviceRemovedExtendedDataSettings1>(this, riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT) override {}
    void STDMETHODCALLTYPE SetPageFaultEnablement(D3D12_DRED_ENABLEMENT) override {}
    void STDMETHODCALLTYPE SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT) override {}
    void STDMETHODCALLTYPE SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT) override {}
};

class DredData final : public ID3D12DeviceRemovedExtendedData2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3D12DeviceRemovedExtendedData, ID3D12DeviceRemovedExtendedData1,
                                ID3D12DeviceRemovedExtendedData2>(this, riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT *) override { return kNotAvailable; }
    HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput(D3D12_DRED_PAGE_FAULT_OUTPUT *) override { return kNotAvailable; }
    HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput1(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 *) override { return kNotAvailable; }
    HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput1(D3D12_DRED_PAGE_FAULT_OUTPUT1 *) override { return kNotAvailable; }
    HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput2(D3D12_DRED_PAGE_FAULT_OUTPUT2 *) override { return kNotAvailable; }
    D3D12_DRED_DEVICE_STATE STDMETHODCALLTYPE GetDeviceState() override { return D3D12_DRED_DEVICE_STATE_UNKNOWN; }
};

} // namespace

ID3D12DeviceRemovedExtendedDataSettings1 *dred_settings()
{
    static DredSettings settings;
    return &settings;
}

ID3D12DeviceRemovedExtendedData2 *dred_data()
{
    static DredData data;
    return &data;
}

} // namespace d3d12m
