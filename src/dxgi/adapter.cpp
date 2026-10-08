#include "dxgi/adapter.h"

#include <algorithm>
#include <iterator>

#include "common/com.h"
#include "common/log.h"
#include "common/luid.h"
#include "common/private_data.h"

#include "bridge/mtlb.h"

namespace d3d12m {

namespace {

constexpr UINT kVendorIdApple = 0x106B;

// Copies the members shared by all three adapter description structs.
template <typename Desc>
void fill_common(Desc &d, const DXGI_ADAPTER_DESC2 &src)
{
    std::copy(std::begin(src.Description), std::end(src.Description), d.Description);
    d.VendorId = src.VendorId;
    d.DeviceId = src.DeviceId;
    d.SubSysId = src.SubSysId;
    d.Revision = src.Revision;
    d.DedicatedVideoMemory = src.DedicatedVideoMemory;
    d.DedicatedSystemMemory = src.DedicatedSystemMemory;
    d.SharedSystemMemory = src.SharedSystemMemory;
    d.AdapterLuid = src.AdapterLuid;
}

class Adapter final : public WithPrivateData<RefCounted<IDXGIAdapter3>> {
public:
    // Takes a reference on `parent`.
    Adapter(IDXGIFactory *parent, const mtlb_device_caps &caps) : parent_(parent)
    {
        parent_->AddRef();

        // Adapter descriptions are UTF-16 on Windows; the device name is ASCII.
        const size_t max_chars = std::size(desc_.Description) - 1;
        for (size_t i = 0; i < max_chars && caps.name[i]; i++)
            desc_.Description[i] = static_cast<unsigned char>(caps.name[i]);

        desc_.VendorId = kVendorIdApple;
        desc_.DedicatedVideoMemory = caps.recommended_max_working_set_size;
        desc_.SharedSystemMemory = caps.recommended_max_working_set_size;
        desc_.AdapterLuid = luid_from_registry_id(caps.registry_id);
    }

    ~Adapter() override { parent_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1,
                                IDXGIAdapter2, IDXGIAdapter3>(this, riid, out);
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override
    {
        return parent_->QueryInterface(riid, parent);
    }

    // IDXGIAdapter
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput **output) override
    {
        if (output)
            *output = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        fill_common(*desc, desc_);
        return S_OK;
    }

    // The layer is a plain translation layer: every interface "is supported".
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *umd_version) override
    {
        if (umd_version)
            umd_version->QuadPart = 1;
        return S_OK;
    }

    // IDXGIAdapter1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        fill_common(*desc, desc_);
        desc->Flags = desc_.Flags;
        return S_OK;
    }

    // IDXGIAdapter2
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
    }

    // IDXGIAdapter3
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD) override
    {
        D3D12M_STUB_LOG();
    }

    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO *info) override
    {
        if (node != 0 || !info)
            return E_INVALIDARG;
        info->Budget = desc_.DedicatedVideoMemory;
        info->CurrentUsage = 0;
        info->AvailableForReservation = desc_.DedicatedVideoMemory / 2;
        info->CurrentReservation = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT node, DXGI_MEMORY_SEGMENT_GROUP, UINT64) override
    {
        return node == 0 ? S_OK : E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE, DWORD *) override
    {
        D3D12M_STUB_HR();
    }

    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD) override
    {
        D3D12M_STUB_LOG();
    }

private:
    IDXGIFactory *parent_;
    DXGI_ADAPTER_DESC2 desc_{}; // GetDesc/GetDesc1 are prefixes of this
};

} // namespace

IDXGIAdapter3 *create_adapter(IDXGIFactory *parent, const mtlb_device_caps &caps)
{
    return new Adapter(parent, caps);
}

} // namespace d3d12m
